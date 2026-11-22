// The slice table the engine plays (spec 02-features/slicing.md SLC-003/004/010/011, audio-engine.md
// "Sample memory": voices read through a SliceMap view, slicing never copies audio). An immutable plain value — at
// most 65 frame offsets — built on the message thread from the track's SliceSettings and its sample's frame count
// (core/commands/slice/SlicePoints.h buildTable, < 10 µs) and installed per scene × track with
// ObjectSwap{SwapSlot::SliceMap, scene, track}; engine/voice/SliceMap.h is the engine's name for it.
//
// Why core/util: tools/check-deps.py lets the engine include core/util but not core/commands, and lets ui / app
// include only engine/EngineApi.h of the engine, so the one type both halves share lives here (like ChainPlan and the
// sequencer's PatternImage). The object is a plain `new SliceTable`, freed by destroySliceTable on the message thread.
//
//   message thread:  auto* t = new SliceTable(core::slice::buildTable(settings, frames));
//                    if (! bridge.post(ObjectSwap{{SwapSlot::SliceMap, scene, track}, t})) destroySliceTable(t);
//
// Pad → region is O(1): slice = 16·window + pad (pad 0 = bottom-left), region = [points[s], points[s + 1]).
#pragma once

#include <array>
#include <cstdint>
#include <type_traits>

namespace core::slice
{
inline constexpr int kTableSlices = 64;

struct SliceRegion // end = the next slice's start (SLC-004)
{
    std::int64_t start = 0, end = 0;
    friend constexpr bool operator== (const SliceRegion&, const SliceRegion&) = default;
};

struct SliceTable
{
    std::int32_t slices = 0;  // 0 (no sample) … 64
    std::int32_t window = 0;  // AREA: the pads play slices 16·window … 16·window + 15 (windowOffset)
    std::int64_t frames = 0;  // the sample's frame count the table was built for (points[slices] = frames)
    std::array<std::int64_t, kTableSlices + 1> points {};

    constexpr int count() const noexcept { return slices; }
    constexpr int windowOffset() const noexcept { return window; }
    // Region of slice `s` (0-based); an empty region for a slice that does not exist.
    constexpr SliceRegion region(int s) const noexcept
    {
        return s >= 0 && s < slices && s < kTableSlices ? SliceRegion {points[static_cast<std::size_t>(s)], points[static_cast<std::size_t>(s + 1)]}
                                    : SliceRegion {};
    }
    // Pad 0–15 → slice 16·windowOffset + pad, −1 when the grid has no such slice.
    constexpr int sliceForPad(int pad, int windowOffset) const noexcept
    {
        if (pad < 0 || pad > 15 || windowOffset < 0)
            return -1;
        const int s = windowOffset * 16 + pad;
        return s < slices ? s : -1;
    }
    // GRID SNAP (SLC-011, PLY-004): the slice point nearest to `frame` (ties: the earlier one); `frame` itself
    // without slices.
    constexpr std::int64_t snap(std::int64_t frame) const noexcept
    {
        if (slices <= 0)
            return frame;
        int lo = 0, hi = slices; // binary search over points[0…slices]
        while (lo < hi)
        {
            const int mid = (lo + hi) / 2;
            if (points[static_cast<std::size_t>(mid)] < frame)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo == 0)
            return points[0];
        const std::int64_t a = points[static_cast<std::size_t>(lo - 1)], b = points[static_cast<std::size_t>(lo)];
        return frame - a <= b - frame ? a : b;
    }
};
static_assert(std::is_trivially_copyable_v<SliceTable>, "the slice table is a plain value");

// SwapTable DestroyFn of SwapSlot::SliceMap (message thread only: the retire queue calls it).
inline void destroySliceTable(const void* table) noexcept
{
    delete static_cast<const SliceTable*>(table); // rt-ok: message thread only (the retire queue frees retired tables)
}
} // namespace core::slice
