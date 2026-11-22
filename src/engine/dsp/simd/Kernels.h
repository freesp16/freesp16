// Hot block kernels of the engine frame: every kernel has a scalar reference
// (`scalar::`) and a NEON version (`neon::`, ARMv7 intrinsics subset, only where __ARM_NEON is defined), tested
// against each other on the host (tests/unit/engine/test_simd.cpp; Apple silicon runs the same intrinsics). The
// unqualified functions dispatch at compile time: NEON when available, else the scalar reference.
//
// Contract: pointers 16-byte aligned is preferred but not required; any n >= 0 (the 4-wide body plus a scalar tail).
// Real-time safe: no allocation, no libm, no branches per sample beyond the loop.
#pragma once

#include <cstddef>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
 #define APP_ENGINE_HAVE_NEON 1
#else
 #define APP_ENGINE_HAVE_NEON 0
#endif

namespace engine::simd
{
// Peak (max |x|) and sum of squares of one channel in one pass (meters, audio-engine.md step 13).
struct PeakSum
{
    float peak = 0.0f;
    float sumSquares = 0.0f;
};

namespace scalar
{
PeakSum peakSum(const float* x, int n) noexcept;
float peakAbs(const float* x, int n) noexcept;
void add(float* dst, const float* src, int n) noexcept;                      // dst += src
void scale(float* dst, const float* src, float g, int n) noexcept;           // dst = src * g
void mulAdd(float* dst, const float* src, const float* g, int n) noexcept;   // dst += src * g[i]
void multiply(float* dst, const float* g, int n) noexcept;                   // dst *= g[i]
} // namespace scalar

#if APP_ENGINE_HAVE_NEON
namespace neon
{
PeakSum peakSum(const float* x, int n) noexcept;
float peakAbs(const float* x, int n) noexcept;
void add(float* dst, const float* src, int n) noexcept;
void scale(float* dst, const float* src, float g, int n) noexcept;
void mulAdd(float* dst, const float* src, const float* g, int n) noexcept;
void multiply(float* dst, const float* g, int n) noexcept;
} // namespace neon
namespace active = neon;
#else
namespace active = scalar;
#endif

inline PeakSum peakSum(const float* x, int n) noexcept { return active::peakSum(x, n); }
inline float peakAbs(const float* x, int n) noexcept { return active::peakAbs(x, n); }
inline void add(float* dst, const float* src, int n) noexcept { active::add(dst, src, n); }
inline void scale(float* dst, const float* src, float g, int n) noexcept { active::scale(dst, src, g, n); }
inline void mulAdd(float* dst, const float* src, const float* g, int n) noexcept { active::mulAdd(dst, src, g, n); }
inline void multiply(float* dst, const float* g, int n) noexcept { active::multiply(dst, g, n); }

constexpr bool hasNeon() noexcept { return APP_ENGINE_HAVE_NEON != 0; }
} // namespace engine::simd
