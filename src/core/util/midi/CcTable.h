// The fixed incoming CC map as plain data (spec 02-features/midi.md MID-030/031/035, scenes-and-patterns.md SCN-050,
// effects/parameters.md FX-086; research docs/midi-implementation.md "Fixed incoming CC map"). Built on the message
// thread from the parameter registry (io/midi/CcTableBuilder: ParamRegistry::byMidiCc + core::fx grids), published
// through MidiHub::setCcTable and read by the audio thread (engine/midi/CcMap). POD: no allocation, no strings.
//
// Scaling (MID-031, our law): Linear = lo + cc·(hi − lo)/127 (Integer/Note/Time types rounded to the nearest integer);
// a bipolar parameter (PAN, PITCH, OFFSET: lo < 0 < hi) is centred instead, 0 → lo, 64 → 0, 127 → hi (lo·(64 − cc)/64
// below 64, hi·(cc − 64)/63 above) and rounded to an integer (PITCH: whole semitones);
// Bands = enumerations and booleans, 0–127 cut into `bands` equal bands (index = cc·bands/128); FX positions map onto
// the current grid of the page position's knob, i = lo + round_half_even(cc·(hi − 1 − lo)/127), v = (i − lo)/(hi − 1 −
// lo) (FX-086, the same arithmetic as core::fx::ccToValue).
#pragma once

#include "core/params/ParamId.h"

#include <array>
#include <cmath>
#include <cstdint>

namespace core::midi
{
enum class CcKind : std::uint8_t
{
    None,          // not mapped: ignored
    Linear,        // a numeric parameter of the track (MID-030/031)
    Bands,         // an enumeration / boolean (MID-031)
    SceneSelect,   // CC 0: scene 1–16 (≥ 16 → 16, 0 ignored; SCN-050)
    PatternSelect, // CC 32: pattern 1–16
    Fx1,           // CC 46–51: page position 1–6 of FX1 (MID-035, FX-086)
    Fx2,           // CC 63–68: page position 1–6 of FX2
    AllNotesOff    // CC 123 (MID-023)
};

struct CcEntry
{
    CcKind kind = CcKind::None;
    std::uint8_t bands = 0;    // Bands: number of values; Fx1/Fx2: page position 0–5
    bool integral = false;     // Linear: round to an integer
    bool bipolar = false;      // Linear: 64 = 0 (lo < 0 < hi)
    core::ParamId param {};    // Linear / Bands: the track-scoped parameter
    float lo = 0.0f, hi = 1.0f;
};

struct FxGrid // [lo, hi) of one FX knob ParamSpec (free grid, or the sync grid of a `.div` id)
{
    std::int32_t lo = 0, hi = 0; // hi <= lo = no grid (not an FX knob)
    friend constexpr bool operator== (FxGrid, FxGrid) = default;
};

struct CcTable
{
    static constexpr std::size_t kMaxParams = 2048; // = engine::bridge::ParamBindings::kMaxParams
    std::array<CcEntry, 128> cc {};
    std::array<FxGrid, kMaxParams> fxGrid {}; // by ParamId.value
};

// ---- the arithmetic (any thread, noexcept) ----

inline float ccLinear(const CcEntry& e, int cc) noexcept
{
    const int c = cc < 0 ? 0 : (cc > 127 ? 127 : cc);
    float v = e.lo + static_cast<float>(c) * (e.hi - e.lo) / 127.0f;
    if (e.bipolar && e.lo < 0.0f && e.hi > 0.0f)
        v = c < 64 ? e.lo * static_cast<float>(64 - c) / 64.0f : e.hi * static_cast<float>(c - 64) / 63.0f;
    if (e.integral)
        v = std::nearbyint(v);
    return v == 0.0f ? 0.0f : v; // −0 → 0
}

inline int ccBand(int bands, int cc) noexcept
{
    if (bands <= 1)
        return 0;
    const int c = cc < 0 ? 0 : (cc > 127 ? 127 : cc);
    const int i = c * bands / 128;
    return i >= bands ? bands - 1 : i;
}

// FX-086: the step fraction for `cc` on grid [lo, hi) (bit-identical to core::fx::ccToValue).
inline float ccFxValue(int cc, FxGrid g) noexcept
{
    const int span = g.hi - 1 - g.lo;
    if (span <= 0)
        return 0.0f;
    const int c = cc < 0 ? 0 : (cc > 127 ? 127 : cc);
    const double x = static_cast<double>(c) * static_cast<double>(span) / 127.0;
    const int i = g.lo + static_cast<int>(std::nearbyint(x)); // half to even (MID-031)
    // core::fx::valueOfStep: v = f32(f32(i − lo) / f32(hi − 1 − lo))
    return static_cast<float>(i - g.lo) / static_cast<float>(span);
}

// CC 0 / CC 32: 1–16 select 0–15, ≥ 16 → 15, 0 → −1 (ignored, SCN-050).
inline int ccSelect(int cc) noexcept { return cc <= 0 ? -1 : (cc >= 16 ? 15 : cc - 1); }
} // namespace core::midi
