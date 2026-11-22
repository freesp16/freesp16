// The arrangement the engine plays in song mode (spec 02-features/arrangement.md ARR-001…008): an immutable plain value
// built on the message thread from the project's Arrangement (core/commands/arrangement/ArrangementPlanBuilder.h)
// and installed with ObjectSwap{SwapSlot::SceneConfig, kSwapScene, kSwapIndex}; engine/scene/SectionSequencer.h plays
// it (step 5).
//
// Why core/util (as core/util/chain/ChainPlan.h): tools/check-deps.py lets the engine include core/util but not
// core/commands, and lets ui / app include only engine/EngineApi.h of the engine, so the type both halves share lives
// here. The object is a plain `new ArrangementPlan` (≈ 16 KB), freed by destroyArrangementPlan on the message thread
// (RetireQueue).
//
// Why SwapSlot::SceneConfig 0/15: SwapSlot::ProjectConfig belongs to the pattern chain (its DestroyFn deletes a
// ChainPlan, and a slot kind has one DestroyFn), and SceneConfig had no user (its slot 0/0 is the engine bridge tests'
// generic payload slot, tests/unit/engine/test_bridge.cpp). Whoever adds per-scene configuration objects later must
// share this kind's destroy function (or give the kind a type tag).
//
//   message thread:  auto* plan = new ArrangementPlan(buildPlan(project));      plan->status = &liveSongStatus();
//                    if (! bridge.post(ObjectSwap{{SwapSlot::SceneConfig, kSwapScene, kSwapIndex}, plan}))
//                        destroyArrangementPlan(plan);
//
// SongStatus (ARR-007, the playing row and its remaining repeats for the ARRANGER screen) is a local adapter
// [contract]: PlayheadFeed would be the natural carrier, but engine/EngineApi.h has no field for it, so the engine
// writes these few relaxed atomics (32-bit: lock-free on the Cortex-A9, no libatomic) once per block through
// plan->status, and the UI reads them. Offline renders leave status null.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <type_traits>

namespace core::arrangement
{
inline constexpr int kMaxRows = 999;   // ARR-001
inline constexpr int kMaxRepeats = 99; // ARR-002
inline constexpr std::uint8_t kSwapScene = 0, kSwapIndex = 15; // SwapSlot::SceneConfig 0/15

// One arranger row: 0-based scene and pattern, repeats 1–99, bpm > 0 = a fixed BPM, ≤ 0 = "--" (keep the tempo).
struct Section
{
    std::uint8_t scene = 0, pattern = 0, repeats = 1;
    float bpm = 0.0f;
};

// The engine's song position, written by the audio thread once per block (relaxed; the UI reads a possibly mixed but
// always valid set: each field is checked on its own). position16 = sixteenths from the arrangement's start.
struct SongStatus
{
    std::atomic<std::int32_t> row {-1};       // the playing row (0-based), −1 = the song does not play
    std::atomic<std::int32_t> remaining {0};  // passes left in that row, the current one included (1 = last pass)
    std::atomic<std::int32_t> position16 {0}; // song position in sixteenths (the SPP value, ARR-008)
    std::atomic<std::int32_t> cued {0};       // the row Play starts from while stopped (SHIFT + push, ARR-004)
};
static_assert(std::atomic<std::int32_t>::is_always_lock_free, "the song status must not need libatomic");

// The live engine's status (one per process; the offline render engine gets none). Message thread only: the first call
// constructs it; the audio thread only writes through ArrangementPlan::status.
inline SongStatus& liveSongStatus() noexcept
{
    static SongStatus status;
    return status;
}

struct ArrangementPlan
{
    std::uint16_t count = 0;                // rows 0…999; 0 = no arrangement (song mode does nothing)
    std::array<Section, kMaxRows> rows {};
    // startPpq16[i] = the sixteenth at which row i starts (from the pattern lengths when this plan was built);
    // startPpq16[count] = the song length. A step is a sixteenth (SEQ-002), so a row lasts repeats × length.
    std::array<std::int64_t, kMaxRows + 1> startPpq16 {};
    SongStatus* status = nullptr;           // where the engine publishes the song position (null = nowhere)

    // Row length of one pass, in sixteenths (the pattern length when this plan was built).
    std::int64_t passLength16(int row) const noexcept
    {
        const auto& r = rows[static_cast<std::size_t>(row)];
        const std::int64_t span = startPpq16[static_cast<std::size_t>(row) + 1] - startPpq16[static_cast<std::size_t>(row)];
        return r.repeats > 0 ? span / r.repeats : span;
    }
    // The row that contains sixteenth `pos` (binary search, ≤ 10 steps; −1 when outside the song).
    int rowAt(std::int64_t pos) const noexcept
    {
        if (count == 0 || pos < 0 || pos >= startPpq16[count])
            return -1;
        int lo = 0, hi = count - 1;
        while (lo < hi)
        {
            const int mid = (lo + hi + 1) / 2;
            if (startPpq16[static_cast<std::size_t>(mid)] <= pos)
                lo = mid;
            else
                hi = mid - 1;
        }
        return lo;
    }
};
static_assert(std::is_trivially_copyable_v<ArrangementPlan>, "the arrangement plan is a plain value");

// SwapTable DestroyFn of the slot (message thread only: the retire queue calls it).
inline void destroyArrangementPlan(const void* plan) noexcept
{
    delete static_cast<const ArrangementPlan*>(plan); // rt-ok: message thread only (the retire queue frees retired plans)
}
} // namespace core::arrangement
