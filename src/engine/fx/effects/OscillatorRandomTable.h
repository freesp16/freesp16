// The app's own random table of the shared modulation oscillator (spec 02-features/effects/oscillator.md FX-1305).
// The reference reads an 8192-value table from firmware data; the app never uses it and designs its own:
//
//   8192 float32 values in [−1, 1), xorshift32 from the seed 0x5EED1600 (x ^= x << 13; x ^= x >> 17; x ^= x << 5;
//   the state is advanced before each value), value = (x >> 8) · 2^-23 − 1 (exact in float32).
//
// The table is compiled in (constant-initialised, read-only) and rebuildable from the formula alone: the unit test
// tests/unit/fx_color/test_oscillator.cpp regenerates it bit for bit, and tools/golden-gen/goldenlib/models/oscillator.py
// rebuilds the same values for the research model osc.py (passed through its `table=` argument, with table[0] as the
// 8193rd word the model's index wrap can reach, our choice).
#pragma once

#include <array>
#include <cstdint>

namespace engine::fx
{
constexpr int kOscRandomTableSize = 8192;
constexpr std::uint32_t kOscRandomSeed = 0x5EED1600u;

constexpr std::uint32_t oscRandomNext(std::uint32_t x) noexcept
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}
constexpr float oscRandomValue(std::uint32_t x) noexcept
{
    return static_cast<float>(x >> 8) * (1.0f / 8388608.0f) - 1.0f;
}

extern const std::array<float, kOscRandomTableSize> kOscRandomTable;
} // namespace engine::fx
