// Vector kernels of the effects framework (DSP kit): copy, gain, per-sample gain, add and the
// non-fused dry/wet mix out = f32(f32(x·d) + f32(c·w)) (FX-030). Every kernel has a scalar reference (`scalar::`) and
// a NEON version (`neon::`, ARMv7 intrinsics subset: vld1q/vst1q/vmulq/vmulq_n/vaddq — never vmlaq/vfmaq), checked
// against each other bit for bit on the host (tests/unit/fx/test_fx_dsp.cpp). The unqualified functions dispatch at
// compile time. Any n >= 0 (4-wide body + scalar tail); pointers need no alignment; in place where documented.
#pragma once

#include "engine/fx/dsp/Numerics.h"

namespace engine::fx::dsp::vec
{
namespace scalar
{
void copy(float* dst, const float* src, int n) noexcept;
void mul(float* x, float g, int n) noexcept;                                         // x *= g
void mulv(float* x, const float* g, int n) noexcept;                                 // x *= g[i]
void add(float* dst, const float* src, int n) noexcept;                              // dst += src
void mix(float* x, const float* c, float d, float w, int n) noexcept;                // x = f32(x·d) + f32(c·w)
void mixv(float* x, const float* c, const float* d, const float* w, int n) noexcept; // per-sample gains
float peakAbs(const float* x, int n) noexcept;
} // namespace scalar

#if APP_FX_HAVE_NEON
namespace neon
{
void copy(float* dst, const float* src, int n) noexcept;
void mul(float* x, float g, int n) noexcept;
void mulv(float* x, const float* g, int n) noexcept;
void add(float* dst, const float* src, int n) noexcept;
void mix(float* x, const float* c, float d, float w, int n) noexcept;
void mixv(float* x, const float* c, const float* d, const float* w, int n) noexcept;
float peakAbs(const float* x, int n) noexcept;
} // namespace neon
namespace active = neon;
#else
namespace active = scalar;
#endif

inline void copy(float* dst, const float* src, int n) noexcept
{
    active::copy(dst, src, n);
}
inline void mul(float* x, float g, int n) noexcept
{
    active::mul(x, g, n);
}
inline void mulv(float* x, const float* g, int n) noexcept
{
    active::mulv(x, g, n);
}
inline void add(float* dst, const float* src, int n) noexcept
{
    active::add(dst, src, n);
}
inline void mix(float* x, const float* c, float d, float w, int n) noexcept
{
    active::mix(x, c, d, w, n);
}
inline void mixv(float* x, const float* c, const float* d, const float* w, int n) noexcept
{
    active::mixv(x, c, d, w, n);
}
inline float peakAbs(const float* x, int n) noexcept
{
    return active::peakAbs(x, n);
}
} // namespace engine::fx::dsp::vec
