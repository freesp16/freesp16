// The slice points of a Sample track (spec 02-features/slicing.md SLC-001…006, SLC-010/011): pure
// integer functions over core::model::SliceSettings and the sample's frame count, shared by the slice commands, the
// engine's SliceMap builder (engine/voice/SliceMap.h) and the SET SLICE / PLAYBACK screens. No allocation.
//
// Model (SLC-001/002, OQ-04; our own encoding): the grid is `gridCount` equal slices over the whole sample (not
// START…END): grid point g (0…N−1) at g·frames / N, rounded down. Only edited points are stored, in
// SliceSettings::grids as flat (tag, frame) pairs — empty for an untouched grid, as in the reference:
//   tag 0…63   grid point `tag` moved to `frame`
//   tag 64     a point added at `frame` (ADD SLICE)
// The list of slice starts is the grid (with its moved points) plus the added points, sorted by frame; a point at or
// past the sample end or on an earlier point's frame is dropped, so slices are never empty. Slice i plays
// [point i, point i+1) with point `count` = the sample end (SLC-004). Grid points can be moved, not deleted; added
// points can be moved and deleted (DELETE SLICE is disabled for slice 1 and for grid points, UID-341). At most
// kMaxSlices points in all (ADD SLICE refuses more).
#pragma once

#include "core/model/Track.h"
#include "core/util/slice/SliceTable.h"

#include <array>
#include <cstdint>

namespace core::slice
{
inline constexpr int kMaxSlices = 64;
inline constexpr int kDefaultCount = 16;
inline constexpr int kPadsPerArea = 16;
inline constexpr std::int64_t kAddedTag = 64;
inline constexpr std::array<int, 6> kGridCounts {2, 4, 8, 16, 32, 64}; // GRID SLICE popup rows (SLC-001)

struct SlicePoint
{
    std::int64_t frame = 0;
    std::int8_t grid = -1; // grid index 0…63, −1 = an added point
    bool moved = false;    // a grid point off its equal position
};

struct SlicePoints
{
    std::array<SlicePoint, kMaxSlices> point {};
    int count = 0;           // slices (= points); 0 without a sample
    std::int64_t frames = 0; // the sample end (point `count`)

    std::int64_t start(int slice) const noexcept { return point[static_cast<std::size_t>(slice)].frame; }
    std::int64_t end(int slice) const noexcept
    {
        return slice + 1 < count ? point[static_cast<std::size_t>(slice + 1)].frame : frames;
    }
};

// A grid count from the popup's list (anything else → the nearest valid one, 16 for ≤ 0).
int validCount(int count) noexcept;
// The grid count `s` slices into: its gridCount when 1…64 (files may hold any), else validCount().
int gridCountOf(const model::SliceSettings& s) noexcept;
// Grid point g of an N-slice grid over `frames` frames.
constexpr std::int64_t gridFrame(int g, int count, std::int64_t frames) noexcept
{
    return count <= 0 || frames <= 0 ? 0 : static_cast<std::int64_t>(g) * frames / count;
}

// The sorted slice starts of `s` over a sample of `frames` frames (frames ≤ 0: count 0).
SlicePoints slicePoints(const model::SliceSettings& s, std::int64_t frames) noexcept;

// AREA (SLC-003): the pads show slices 16·area … 16·area + 15; areas = ⌈count / 16⌉ (at least 1).
int areaCount(int slices) noexcept;
int clampArea(int area, int slices) noexcept;
// Pad 0 (bottom-left) … 15 → slice index 16·area + pad, −1 when that slice does not exist.
int sliceForPad(int pad, int area, int slices) noexcept;

// The engine's table (core/util/slice/SliceTable.h) for `s` over `frames` frames, window = the clamped AREA.
SliceTable buildTable(const model::SliceSettings& s, std::int64_t frames) noexcept;

// Number of stored edits (moved grid points + added points): the "Reset N moved slice points?" message.
int editedPoints(const model::SliceSettings& s) noexcept;
} // namespace core::slice
