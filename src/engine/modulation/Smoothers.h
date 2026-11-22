// Per-block helpers of the track LFO: the sine of a phase in turns without libm, and the one-pole block
// smoother of LFO-011. Header-only, RT-safe; each call is a handful of flops (no libm per block).
//
// sinTurns: quadrant reduction + an odd Taylor polynomial of degree 11 on [0, π/2] (truncation ≤ 6e−8, float evaluation
// ≤ 2e−7 in all). Not a 1024-point table with linear interpolation: that table's
// interpolation error is (2π/1024)²/8 ≈ 4.7e−6, above the acceptance's 1e−6 against the analytic sine, and its 4 KiB
// would be touched at 16 random places per block; the polynomial is exact enough and touches no memory.
#pragma once

#include <cstdint>

namespace engine::modulation
{
// sin(2π · phase) for phase in turns, |phase| < 2^31 (reduced to [0, 1)).
inline float sinTurns(double phase) noexcept
{
    double p = phase - static_cast<double>(static_cast<std::int32_t>(phase)); // |phase| < 2^31 (int32: one VFP op)
    if (p < 0.0)
        p += 1.0;
    const double x = p * 4.0;
    const int q = static_cast<int>(x); // quadrant 0…3 (x < 4)
    float f = static_cast<float>(x - static_cast<double>(q));
    if ((q & 1) != 0)
        f = 1.0f - f; // quadrants 1 and 3 mirror
    const float u = f * 1.57079632679489662f;
    const float u2 = u * u;
    // u − u³/3! + u⁵/5! − u⁷/7! + u⁹/9! − u¹¹/11!
    const float s = u * (1.0f + u2 * (-1.66666667e-1f + u2 * (8.33333333e-3f + u2 * (-1.98412698e-4f +
                                                                                     u2 * (2.75573192e-6f + u2 * -2.50521084e-8f)))));
    return q >= 2 ? -s : s;
}

// One-pole lowpass applied once per block (LFO-011: 50 Hz, the shared oscillator's default smoother):
// y += (1 − a)(x − y), a = exp(−2π · 50 · 64 / fs), computed by the owner in prepare().
struct BlockSmoother
{
    float y = 0.0f;
    float k = 1.0f; // 1 − a; 1 = no smoothing

    float process(float x) noexcept
    {
        y += k * (x - y);
        return y;
    }
    void reset(float v) noexcept { y = v; }
};
} // namespace engine::modulation
