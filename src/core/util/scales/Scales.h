// Pad scales (spec 02-features/pads-and-scales.md PAD-010…013, PAD-020): the 14 scale degree masks and the
// pad → semitone / pad → MIDI note maps. Pure constexpr C++20, no allocation, no libm: the engine (TriggerRouter, SCALE
// pad mode), the SET SCALE screen (pad labels, root shading) and the MIDI engine (MIDI tracks) share it.
//
//   mask        12-bit degree mask, bit i = the scale contains the semitone i above its root; the order is the spec's
//               own list (Chromatic first, OQ-10) = core::model::ScaleType = enumerations.md "Scales"
//   pad         0–15, pad 1 (index 0) bottom-left = the root, rising to the top-right (PAD-013)
//   window      `windowOffset` / `scalePosition`: the pads cover the scale degrees [16·w, 16·w + 16) (PAD-012,
//               paging by 16 degrees) — negative windows reach below the root
//   keyShift    −12…+12 semitones, transposes the root (PAD-011)
//
// padSemitones(pad, …) = 12·⌊d / n⌋ + interval[d mod n] + keyShift with d = pad + 16·window and n = degrees of the
// scale; the voice adds it in cents to the track's pitch (PLY-022). midiPadNote() is the same degree map from a MIDI
// root note, clamped to 0–127.
#pragma once

#include <array>
#include <cstdint>

namespace core::scales
{
constexpr int kScales = 14;
constexpr int kPads = 16;
constexpr int kMinKeyShift = -12, kMaxKeyShift = 12;
constexpr int kMinWindow = -2, kMaxWindow = 2; // ±2 pages of 16 degrees (ours) (the SET SCALE map shows 64 degrees)

// Chromatic, Major, Natural minor, Harmonic minor, Melodic minor, Dorian, Phrygian, Lydian, Mixolydian, Locrian,
// Major pentatonic, Minor pentatonic, Blues, Whole tone (PAD-010).
constexpr std::array<std::uint16_t, kScales> kScaleMasks = {
    0xFFF,                                                                        // Chromatic
    1u << 0 | 1u << 2 | 1u << 4 | 1u << 5 | 1u << 7 | 1u << 9 | 1u << 11,         // Major
    1u << 0 | 1u << 2 | 1u << 3 | 1u << 5 | 1u << 7 | 1u << 8 | 1u << 10,         // Natural minor
    1u << 0 | 1u << 2 | 1u << 3 | 1u << 5 | 1u << 7 | 1u << 8 | 1u << 11,         // Harmonic minor
    1u << 0 | 1u << 2 | 1u << 3 | 1u << 5 | 1u << 7 | 1u << 9 | 1u << 11,         // Melodic minor
    1u << 0 | 1u << 2 | 1u << 3 | 1u << 5 | 1u << 7 | 1u << 9 | 1u << 10,         // Dorian
    1u << 0 | 1u << 1 | 1u << 3 | 1u << 5 | 1u << 7 | 1u << 8 | 1u << 10,         // Phrygian
    1u << 0 | 1u << 2 | 1u << 4 | 1u << 6 | 1u << 7 | 1u << 9 | 1u << 11,         // Lydian
    1u << 0 | 1u << 2 | 1u << 4 | 1u << 5 | 1u << 7 | 1u << 9 | 1u << 10,         // Mixolydian
    1u << 0 | 1u << 1 | 1u << 3 | 1u << 5 | 1u << 6 | 1u << 8 | 1u << 10,         // Locrian
    1u << 0 | 1u << 2 | 1u << 4 | 1u << 7 | 1u << 9,                              // Major pentatonic
    1u << 0 | 1u << 3 | 1u << 5 | 1u << 7 | 1u << 10,                             // Minor pentatonic
    1u << 0 | 1u << 3 | 1u << 5 | 1u << 6 | 1u << 7 | 1u << 10,                   // Blues
    1u << 0 | 1u << 2 | 1u << 4 | 1u << 6 | 1u << 8 | 1u << 10,                   // Whole tone
};

// The mask of scale `index` (0–13); out of range → Chromatic.
constexpr std::uint16_t scaleMask(int index) noexcept
{
    return index >= 0 && index < kScales ? kScaleMasks[static_cast<std::size_t>(index)] : kScaleMasks[0];
}

// Degrees per octave of a mask (the root always counts, so never 0).
constexpr int degreeCount(std::uint16_t mask) noexcept
{
    mask = static_cast<std::uint16_t>((mask & 0xFFFu) | 1u);
    int n = 0;
    for (int i = 0; i < 12; ++i)
        n += (mask >> i) & 1u;
    return n;
}

// Semitones of scale degree `degree` (any integer; negative = below the root) above the root.
constexpr int degreeSemitones(int degree, std::uint16_t mask) noexcept
{
    mask = static_cast<std::uint16_t>((mask & 0xFFFu) | 1u);
    const int n = degreeCount(mask);
    int octave = degree >= 0 ? degree / n : -((-degree + n - 1) / n); // floor division
    int k = degree - octave * n;                                       // 0 … n−1
    for (int i = 0; i < 12; ++i)
        if ((mask >> i) & 1u)
        {
            if (k == 0)
                return octave * 12 + i;
            --k;
        }
    return octave * 12; // unreachable
}

constexpr int clampInt(int v, int lo, int hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

// PAD-011…013: the semitone offset pad `pad` (0–15) plays in SCALE pad mode.
constexpr int padSemitones(int pad, std::uint16_t scaleMask, int keyShift, int windowOffset) noexcept
{
    const int d = clampInt(pad, 0, kPads - 1) + kPads * clampInt(windowOffset, kMinWindow, kMaxWindow);
    return degreeSemitones(d, scaleMask) + clampInt(keyShift, kMinKeyShift, kMaxKeyShift);
}

// True when the pad plays an octave of the root (PAD-013 / UI-152: root pads in the brighter shade).
constexpr bool isRootPad(int pad, std::uint16_t scaleMask, int windowOffset) noexcept
{
    const int n = degreeCount(scaleMask);
    const int d = clampInt(pad, 0, kPads - 1) + kPads * clampInt(windowOffset, kMinWindow, kMaxWindow);
    return ((d % n) + n) % n == 0;
}

// PAD-020: the MIDI note pad `pad` plays on a MIDI track (root note 0–127, scale position = window), 0–127.
constexpr int midiPadNote(int pad, std::uint16_t scaleMask, int rootNote, int scalePosition) noexcept
{
    const int d = clampInt(pad, 0, kPads - 1) + kPads * clampInt(scalePosition, kMinWindow, kMaxWindow);
    return clampInt(clampInt(rootNote, 0, 127) + degreeSemitones(d, scaleMask), 0, 127);
}

// The 16 pads' semitone offsets of one scale setting (the engine keeps one table per [scene][track]).
constexpr std::array<std::int8_t, kPads> padTable(std::uint16_t scaleMask, int keyShift, int windowOffset) noexcept
{
    std::array<std::int8_t, kPads> t {};
    for (int p = 0; p < kPads; ++p)
        t[static_cast<std::size_t>(p)] = static_cast<std::int8_t>(clampInt(padSemitones(p, scaleMask, keyShift, windowOffset), -127, 127));
    return t;
}

static_assert(degreeCount(kScaleMasks[0]) == 12 && degreeCount(kScaleMasks[1]) == 7 && degreeCount(kScaleMasks[10]) == 5);
static_assert(padSemitones(0, kScaleMasks[0], 0, 0) == 0 && padSemitones(15, kScaleMasks[0], 0, 0) == 15);
static_assert(padSemitones(7, kScaleMasks[1], 0, 0) == 12 && padSemitones(0, kScaleMasks[1], 0, -1) == -27); // degree −16 = 3 octaves down + 6th
} // namespace core::scales
