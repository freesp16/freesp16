// Pad scales (spec 02-features/pads-and-scales.md PAD-010…013, PAD-020): the 22 scale degree masks and the
// pad → semitone / pad → MIDI note maps. Pure constexpr C++20, no allocation, no libm: the engine (TriggerRouter, SCALE
// pad mode), the SET SCALE screen (pad labels, root shading) and the MIDI engine (MIDI tracks) share it.
//
//   mask        12-bit degree mask, bit i = the scale contains the semitone i above its root; the order is the
//               manual's SET SCALE list (No. 1–22, Chromatic first) = core::model::ScaleType
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
#include <initializer_list>

namespace core::scales
{
constexpr int kScales = 22;
constexpr int kPads = 16;
constexpr int kMinKeyShift = -12, kMaxKeyShift = 12;
constexpr int kMinWindow = -2, kMaxWindow = 2; // ±2 pages of 16 degrees (ours) (the SET SCALE map shows 64 degrees)

// The manual's SET SCALE list ("Selection of scale"); intervals as core::model::scaleIntervals (test_scales pins both).
constexpr std::uint16_t maskOf(std::initializer_list<int> semitones) noexcept
{
    std::uint16_t m = 0;
    for (const int s : semitones)
        m = static_cast<std::uint16_t>(m | (1u << s));
    return m;
}
constexpr std::array<std::uint16_t, kScales> kScaleMasks = {
    0xFFF,                             // Chromatic
    maskOf({0, 2, 4, 5, 7, 9, 11}),    // Ionian
    maskOf({0, 2, 3, 5, 7, 9, 10}),    // Dorian
    maskOf({0, 1, 3, 5, 7, 8, 10}),    // Phrygian
    maskOf({0, 2, 4, 6, 7, 9, 11}),    // Lydian
    maskOf({0, 2, 4, 5, 7, 9, 10}),    // Mixolydian
    maskOf({0, 2, 3, 5, 7, 8, 10}),    // Aeolian
    maskOf({0, 1, 3, 5, 6, 8, 10}),    // Locrian
    maskOf({0, 2, 4, 7, 9}),           // Major Pentatonic
    maskOf({0, 3, 5, 7, 10}),          // Minor Pentatonic
    maskOf({0, 2, 4, 6, 8, 10}),       // Whole Tone
    maskOf({0, 2, 3, 5, 6, 8, 9, 11}), // Diminish (whole-half)
    maskOf({0, 1, 3, 4, 6, 7, 9, 10}), // Combination Diminish (half-whole)
    maskOf({0, 1, 3, 4, 6, 8, 10}),    // Altered
    maskOf({0, 2, 3, 4, 7, 9}),        // Major Blues
    maskOf({0, 3, 5, 6, 7, 10}),       // Minor Blues
    maskOf({0, 1, 4, 5, 7, 8, 11}),    // Raga Bhairav
    maskOf({0, 1, 4, 6, 7, 9, 11}),    // Raga Gamanasrama
    maskOf({0, 1, 3, 6, 7, 8, 11}),    // Raga Todi
    maskOf({0, 2, 3, 5, 7, 9, 11}),    // Hawaiian
    maskOf({0, 4, 5, 7, 11}),          // Ryukyu
    maskOf({0, 1, 5, 7, 8}),           // Japanese Miyakobushi
};

// The mask of scale `index` (0–21); out of range → Chromatic.
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

// The degree of `mask` that lies `semitones` above the root (any octave), or kNoDegree when the scale lacks that note.
constexpr int kNoDegree = -100000;
constexpr int degreeOf(int semitones, std::uint16_t mask) noexcept
{
    mask = static_cast<std::uint16_t>((mask & 0xFFFu) | 1u);
    const int octave = semitones >= 0 ? semitones / 12 : -((-semitones + 11) / 12); // floor division
    const int within = semitones - octave * 12;                                      // 0 … 11
    if (((mask >> within) & 1u) == 0)
        return kNoDegree;
    int index = 0;
    for (int i = 0; i < within; ++i)
        index += (mask >> i) & 1u;
    return octave * degreeCount(mask) + index;
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

static_assert(degreeCount(kScaleMasks[0]) == 12 && degreeCount(kScaleMasks[1]) == 7 &&
              degreeCount(kScaleMasks[8]) == 5 && degreeCount(kScaleMasks[11]) == 8 &&
              kScaleMasks[19] == maskOf({0, 2, 3, 5, 7, 9, 11}));
static_assert(degreeOf(12, kScaleMasks[1]) == 7 && degreeOf(-1, kScaleMasks[1]) == -1 && degreeOf(1, kScaleMasks[1]) == kNoDegree);
static_assert(padSemitones(0, kScaleMasks[0], 0, 0) == 0 && padSemitones(15, kScaleMasks[0], 0, 0) == 15);
static_assert(padSemitones(7, kScaleMasks[1], 0, 0) == 12 && padSemitones(0, kScaleMasks[1], 0, -1) == -27); // degree −16 = 3 octaves down + 6th
} // namespace core::scales
