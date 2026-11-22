// The track LFO's value laws, shared by the parameter catalogue (core/params/catalogue/lfo.cpp: captions), the engine
// (engine/modulation/Lfo*) and the LFO screen (SPEED caption in SYNC, the graph): spec 02-features/lfo.md
// LFO-002/003/004/008, 03-ui/enumerations.md "LFO". Pure, header-only, RT-safe (no allocation; speedHz uses libm and
// is called on parameter changes only, never per sample).
//
//   lfo.speed        knob 0–127 (CC 60, lock slot 18). Free: 0.01 · 4000^(v/127) Hz, an exponential knob from 0.01 Hz
//                    to 40 Hz (LFO-002, our law). SYNC: the knob picks one of the 17 divisions below in 17 equal zones,
//                    slow → fast like the free knob (16 bars … 1/32T).
//   lfo.depth        0–127 → depth/127 of the destination's normalised range, bipolar (LFO-003)
//   lfo.phase        0–127 → v/128 of a cycle (0°…357.2°, so every value is a distinct start phase) (LFO-004)
//   lfo.randomStart  0–127 → the xorshift32 seed of the Random shape (LFO-008): the same number, the same sequence
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace core::lfo
{
constexpr float kMinHz = 0.01f, kMaxHz = 40.0f;
constexpr float kKnobMax = 127.0f;

inline float speedHz(float knob) noexcept
{
    const float v = knob < 0.0f ? 0.0f : (knob > kKnobMax ? kKnobMax : knob);
    return kMinHz * std::exp2(v / kKnobMax * 11.965784284662087f); // log2(4000): 0 → 0.01 Hz, 127 → 40 Hz
}

// The SYNC divisions (03-ui/enumerations.md "Speed, sync"; ours), longest first: quarters per LFO cycle and caption.
struct SyncDivision
{
    double quarters;
    std::string_view label;
};
inline constexpr std::array<SyncDivision, 17> kSyncDivisions {{
    {64.0, "16 BARS"},      {32.0, "8 BARS"},     {16.0, "4 BARS"},       {8.0, "2 BARS"}, {4.0, "1 BAR"},
    {2.0, "1/2"},           {1.5, "1/4."},        {1.0, "1/4"},           {0.75, "1/8."},  {2.0 / 3.0, "1/4T"},
    {0.5, "1/8"},           {0.375, "1/16."},     {1.0 / 3.0, "1/8T"},    {0.25, "1/16"},  {1.0 / 6.0, "1/16T"},
    {0.125, "1/32"},        {1.0 / 12.0, "1/32T"},
}};
constexpr int kNumSyncDivisions = static_cast<int>(kSyncDivisions.size());

// The division a SPEED knob value selects in SYNC.
constexpr int syncDivisionIndex(float knob) noexcept
{
    const float v = knob < 0.0f ? 0.0f : (knob > kKnobMax ? kKnobMax : knob);
    const int i = static_cast<int>(v * static_cast<float>(kNumSyncDivisions) / (kKnobMax + 1.0f));
    return i < kNumSyncDivisions ? i : kNumSyncDivisions - 1;
}
// The lowest knob value of a division (the value the UI writes when it steps through the divisions).
constexpr float syncDivisionKnob(int index) noexcept
{
    const int i = index < 0 ? 0 : (index >= kNumSyncDivisions ? kNumSyncDivisions - 1 : index);
    const int k = (i * 128 + kNumSyncDivisions - 1) / kNumSyncDivisions; // ceil(i · 128 / 17)
    return static_cast<float>(k);
}

constexpr float depth01(float v) noexcept { return (v < 0.0f ? 0.0f : (v > kKnobMax ? kKnobMax : v)) / kKnobMax; }
constexpr float phase01(float v) noexcept { return (v < 0.0f ? 0.0f : (v > kKnobMax ? kKnobMax : v)) / 128.0f; }

// A non-zero xorshift32 state for a Random Start number (splitmix-style scramble, so neighbouring numbers differ).
constexpr std::uint32_t seedOf(int randomStart) noexcept
{
    std::uint32_t x = static_cast<std::uint32_t>(randomStart) * 0x9E3779B9u + 0x7F4A7C15u;
    x = (x ^ (x >> 16)) * 0x85EBCA6Bu;
    x = (x ^ (x >> 13)) * 0xC2B2AE35u;
    x ^= x >> 16;
    return x != 0 ? x : 0x6D2B79F5u;
}
// One xorshift32 step (Marsaglia 13/17/5): the Random shape's sequence.
constexpr std::uint32_t xorshift32(std::uint32_t x) noexcept
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}
// A state as a value in [−1, 1] (24 significant bits, exact in float).
constexpr float bipolar(std::uint32_t x) noexcept
{
    return static_cast<float>(static_cast<std::int32_t>(x >> 8) - 0x800000) / 8388608.0f;
}
} // namespace core::lfo
