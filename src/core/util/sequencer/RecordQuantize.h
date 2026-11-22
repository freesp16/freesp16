// Record quantise (sequencer.md SEQ-042; the BPM/QUANTIZE popup's QUANTIZE, `seq.recordQuantize`) and the pure step
// arithmetic the engine's recorder and the message-thread commands share.
// RT-safe, header-only, no allocation.
//
// The model's `ProjectSettings::padQuantize` is a core::model::QuantizeUnit (Off/Step/Beat/Bar/Pattern), which
// cannot hold 1/32, 1/16T, 1/8T or 1/8 — record quantise has its own enum here; the model field stays unused by the
// sequencer until the model gets a field for it (a known model gap).
//
// Units: a step is a 1/16 note (SEQ-002); offsets are in 1/24 step (SEQ-005, −24…+24 = ±1 step), so every record
// grid is exact in 1/24 steps: 1/32 = 12, 1/16T = 16, 1/16 = 24, 1/8T = 32, 1/8 = 48.
#pragma once

#include <cmath>
#include <cstdint>

namespace core::sequencer
{
enum class RecordQuantize : std::uint8_t { Off, ThirtySecond, SixteenthTriplet, Sixteenth, EighthTriplet, Eighth };
constexpr int kRecordQuantizeCount = 6;
constexpr RecordQuantize kDefaultRecordQuantize = RecordQuantize::Sixteenth; // SEQ-042 default 1/16

// Grid of a record quantise value in 1/24 steps (Off = 1: the finest offset resolution).
constexpr int gridIn24ths(RecordQuantize q) noexcept
{
    switch (q)
    {
        case RecordQuantize::ThirtySecond: return 12;
        case RecordQuantize::SixteenthTriplet: return 16;
        case RecordQuantize::Sixteenth: return 24;
        case RecordQuantize::EighthTriplet: return 32;
        case RecordQuantize::Eighth: return 48;
        case RecordQuantize::Off: break;
    }
    return 1;
}

constexpr RecordQuantize recordQuantizeOf(int index) noexcept
{
    return index <= 0 ? RecordQuantize::Off
                      : (index >= kRecordQuantizeCount - 1 ? RecordQuantize::Eighth : static_cast<RecordQuantize>(index));
}

// Where a recorded hit is stored: step 0…length−1 of the pass the quantised point falls in, plus its offset.
struct RecordedPosition
{
    int step = 0;
    int offset24 = 0;  // −24…+24
    bool wrapped = false; // the quantised point is the next pass's step 1
};

// `pos` = the hit's position in steps from the start of the current pass, 0 ≤ pos < length. `swingSteps` = the swing
// delay of odd steps in steps (0–0.5). Quantised grids snap to the straight grid; the trigger is stored on the step
// whose line is at or before the grid point, the rest as its offset (1/16T: the second triplet of a beat = step 1 +
// 16/24). Swing is not compensated — quantised recordings swing like step entry. OFF stores the exact timing as the
// nearest step + offset, compensated for that step's swing so the playback lands on the hit (±1/48 step).
inline RecordedPosition quantizeHit(double pos, int length, RecordQuantize q, double swingSteps) noexcept
{
    RecordedPosition r;
    if (length < 1)
        length = 1;
    if (q == RecordQuantize::Off)
    {
        int step = static_cast<int>(std::floor(pos + 0.5));
        const double swing = (step & 1) != 0 ? swingSteps : 0.0;
        int off = static_cast<int>(std::lround((pos - step - swing) * 24.0));
        off = off < -24 ? -24 : (off > 24 ? 24 : off);
        if (step >= length)
        {
            step -= length;
            r.wrapped = true;
        }
        r.step = step;
        r.offset24 = off;
        return r;
    }
    const int grid = gridIn24ths(q);
    const long point = std::lround(pos * 24.0 / grid) * grid; // in 1/24 steps
    long step = point / 24;
    r.offset24 = static_cast<int>(point - step * 24);
    if (step >= length)
    {
        step -= length;
        r.wrapped = true;
    }
    r.step = static_cast<int>(step);
    return r;
}

// Stored trigger codes of the reference step record (SEQ-011, OQ-09): FULL 15, HALF 11, PARAM 8.
constexpr std::int16_t kTriggerCodeFull = 15, kTriggerCodeHalf = 11, kTriggerCodeLockOnly = 8;
} // namespace core::sequencer
