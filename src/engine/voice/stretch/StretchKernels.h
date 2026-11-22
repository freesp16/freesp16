// Per-sample / per-bin kernels of the time-stretch backends. Every kernel has a scalar
// reference (`scalar::`) and a NEON version (`neon::`, ARMv7 intrinsics, only where __ARM_NEON is defined); the
// unqualified functions dispatch at compile time. tests/unit/stretch/test_stretch_kernels.cpp compares them (it SKIPs
// the NEON half on an x86-64 host; scripts/neon-tests.sh runs it under qemu-arm as a Cortex-A9).
//
// The scalar references keep the NEON order of operations where it is cheap to do so (dot(): four lane sums, then
// (s0 + s2) + (s1 + s3), then the tail), so the two agree to rounding even where the compiler reassociates
// (-funsafe-math-optimizations on the device for engine/voice/**); the NEON reciprocal of atan2 (vrecpe + two
// Newton steps) differs from the scalar division by an ulp, so the parity test uses a tolerance of 1e-6.
//
// Real-time safe: no allocation, no libm, n >= 0 (a 4-wide body plus a scalar tail), any alignment.
#pragma once

#include "engine/dsp/simd/Kernels.h" // APP_ENGINE_HAVE_NEON

namespace engine::voice::stretch::kernels
{
constexpr double kPiD = 3.14159265358979323846; // M_PI is not standard C++
constexpr float kPi = 3.14159265358979f;
constexpr float kTwoPi = 6.28318530717959f;

// Wraps a phase into [−π, π] (any finite input; two multiplies, no libm).
inline float princarg(float p) noexcept
{
    const float k = p * (1.0f / kTwoPi);
    const float r = static_cast<float>(static_cast<int>(k + (k >= 0.0f ? 0.5f : -0.5f)));
    return p - r * kTwoPi;
}

// sin and cos of θ in [−π, π] (|error| < 4e-7): quadrant reduction + Taylor polynomials on [−π/4, π/4].
inline void sinCos(float theta, float& s, float& c) noexcept
{
    const float qf = theta * (2.0f / kPi);
    const int q = static_cast<int>(qf + (qf >= 0.0f ? 0.5f : -0.5f));
    const float r = theta - static_cast<float>(q) * (kPi * 0.5f);
    const float r2 = r * r;
    const float sr = r * (1.0f + r2 * (-1.0f / 6.0f + r2 * (1.0f / 120.0f + r2 * (-1.0f / 5040.0f))));
    const float cr = 1.0f + r2 * (-0.5f + r2 * (1.0f / 24.0f + r2 * (-1.0f / 720.0f + r2 * (1.0f / 40320.0f))));
    switch (q & 3)
    {
        case 0: s = sr; c = cr; break;
        case 1: s = cr; c = -sr; break;
        case 2: s = -sr; c = -cr; break;
        default: s = -cr; c = sr; break;
    }
}

namespace scalar
{
void mulWindow(float* dst, const float* src, const float* win, int n) noexcept;   // dst = src · win
void addWindowed(float* acc, const float* src, const float* win, int n) noexcept; // acc += src · win
float dot(const float* a, const float* b, int n) noexcept;                       // Σ a·b (lane order, see top)
// One radix-2 stage of an in-place complex FFT (structure of arrays): for every block of 2·half points,
// t = x[j + half]·w[j]; x[j + half] = x[j] − t; x[j] += t. w (wr, wi) holds `half` twiddles; n = total points.
void butterflies(float* re, float* im, const float* wr, const float* wi, int half, int n) noexcept;
void magSq(float* out, const float* re, const float* im, int n) noexcept;          // re² + im²
void atan2(float* out, const float* y, const float* x, int n) noexcept;            // |error| < 1e-6 rad; (0, 0) → 0
void rotate(float* re, float* im, const float* cr, const float* ci, int n) noexcept; // (re, im) ·= (cr, ci)
} // namespace scalar

#if APP_ENGINE_HAVE_NEON
namespace neon
{
void mulWindow(float* dst, const float* src, const float* win, int n) noexcept;
void addWindowed(float* acc, const float* src, const float* win, int n) noexcept;
float dot(const float* a, const float* b, int n) noexcept;
void butterflies(float* re, float* im, const float* wr, const float* wi, int half, int n) noexcept;
void magSq(float* out, const float* re, const float* im, int n) noexcept;
void atan2(float* out, const float* y, const float* x, int n) noexcept;
void rotate(float* re, float* im, const float* cr, const float* ci, int n) noexcept;
} // namespace neon
namespace active = neon;
#else
namespace active = scalar;
#endif

inline void mulWindow(float* d, const float* s, const float* w, int n) noexcept { active::mulWindow(d, s, w, n); }
inline void addWindowed(float* a, const float* s, const float* w, int n) noexcept { active::addWindowed(a, s, w, n); }
inline float dot(const float* a, const float* b, int n) noexcept { return active::dot(a, b, n); }
inline void butterflies(float* re, float* im, const float* wr, const float* wi, int half, int n) noexcept
{
    active::butterflies(re, im, wr, wi, half, n);
}
inline void magSq(float* o, const float* re, const float* im, int n) noexcept { active::magSq(o, re, im, n); }
inline void atan2(float* o, const float* y, const float* x, int n) noexcept { active::atan2(o, y, x, n); }
inline void rotate(float* re, float* im, const float* cr, const float* ci, int n) noexcept
{
    active::rotate(re, im, cr, ci, n);
}
} // namespace engine::voice::stretch::kernels
