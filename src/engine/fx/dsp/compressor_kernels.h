// Compressor kernels (spec 02-features/effects/compressor.md FX-1120/1123/1125; research
// docs/effects/compressor.md, CC BY 4.0; arithmetic operation for operation as the MIT model tools/fx/compressor.py).
// The per-block scalar work (gain computer, target tracking, the geometric ramp and the slew limiter, which are serial
// recurrences) lives in engine/fx/effects/Compressor.cpp; these are its vectorisable stages, each with a scalar
// reference (`scalar::`) and a NEON version (`neon::`, ARMv7 subset: vabsq/vmaxq/vcgtq/vbslq/vmulq/vaddq — never
// vmlaq/vfmaq), bit for bit equal on normal (finite) inputs (tests/unit/fx/test_fx_compressor.cpp):
//
//   peakDetect   the peak detector 0xf8b88: level = max over the block of max(|L|, |R|), floored at 1e-8, and the
//                1-based position of its FIRST occurrence (the attack ramp length, FX-1120/1160)
//   applyGain    out = f32(g[i]·in[i]) on both channels (FX-1123 "out = gain·in")
//   crossfade    the look-ahead mix a = f32(f32(d·c) + f32(x·f)) with per-sample c = 1 − f, f (FX-1125), non-fused
//
// Hot helpers are inline (the device ABI is softfp: float arguments travel in core registers).
#pragma once

#include "engine/fx/dsp/Numerics.h"

#if APP_FX_HAVE_NEON
#include <arm_neon.h>
#endif

namespace engine::fx::dsp::compressor
{
constexpr float kLevelFloor = 1e-8f; // 0x322bcc77 (FX-1120)

namespace scalar
{
// Returns the level; pos = 1-based in-block position of the first maximum (1 when n <= 0).
inline float peakDetect(const float* l, const float* r, int n, int& pos) noexcept
{
    float peak = 0.0f;
    pos = 1;
    for (int i = 0; i < n; ++i)
    {
        float a = l[i], b = r[i];
        if (a <= 0.0f)
            a = -a;
        if (b <= 0.0f)
            b = -b;
        if (a <= b)
            a = b;
        if (a < kLevelFloor)
            a = kLevelFloor;
        if (peak < a)
        {
            pos = i + 1;
            peak = a;
        }
    }
    return peak;
}

inline void applyGain(const float* g, const float* inL, const float* inR, float* outL, float* outR, int n) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        outL[i] = g[i] * inL[i];
        outR[i] = inR[i] * g[i];
    }
}

// a[i] = f32(f32(d[i]·c[i]) + f32(x[i]·f[i])), in place on a (a may alias d).
inline void crossfade(float* a, const float* d, const float* x, const float* c, const float* f, int n) noexcept
{
    APP_FX_NOFUSE
    for (int i = 0; i < n; ++i)
    {
        const float p = d[i] * c[i];
        const float q = x[i] * f[i];
        a[i] = p + q;
    }
}
} // namespace scalar

#if APP_FX_HAVE_NEON
namespace neon
{
// End of the 4-wide part: the loops run [0, nv) in quads and [nv, n) one by one, two loops with their own induction
// variables and fixed bounds. (One index carried from the quad loop into the tail made GCC 11 at -O3 derive an
// unbounded tail trip count once the kernels were inlined with n = 64: -Waggressive-loop-optimizations.) n <= 0 runs
// neither loop, as before.
constexpr int vectorEnd(int n) noexcept
{
    return n > 0 ? n & ~3 : 0;
}

inline float peakDetect(const float* l, const float* r, int n, int& pos) noexcept
{
    // Per lane: the largest value and the index of its first occurrence (strictly greater replaces), then across lanes
    // the largest value with the smallest index — the scalar loop's result for finite input.
    const int nv = vectorEnd(n);
    const float32x4_t floor = vdupq_n_f32(kLevelFloor);
    float32x4_t best = vdupq_n_f32(0.0f);
    const int init[4] = {0, 1, 2, 3};
    int32x4_t idx = vld1q_s32(init);
    int32x4_t bestIdx = idx;
    const int32x4_t four = vdupq_n_s32(4);
    for (int i = 0; i < nv; i += 4)
    {
        const float32x4_t a = vabsq_f32(vld1q_f32(l + i));
        const float32x4_t b = vabsq_f32(vld1q_f32(r + i));
        const float32x4_t m = vmaxq_f32(vmaxq_f32(a, b), floor);
        const uint32x4_t gt = vcgtq_f32(m, best);
        best = vbslq_f32(gt, m, best);
        bestIdx = vbslq_s32(gt, idx, bestIdx);
        idx = vaddq_s32(idx, four);
    }
    float bv[4];
    int bi[4];
    vst1q_f32(bv, best);
    vst1q_s32(bi, bestIdx);
    float peak = 0.0f;
    int at = -1;
    if (nv > 0)
        for (int k = 0; k < 4; ++k)
            if (bv[k] > peak || (bv[k] == peak && at >= 0 && bi[k] < at))
            {
                peak = bv[k];
                at = bi[k];
            }
    pos = at >= 0 ? at + 1 : 1;
    for (int i = nv; i < n; ++i) // tail (n % 4 != 0): later samples win only when strictly greater
    {
        float a = absf(l[i]), b = absf(r[i]);
        if (a <= b)
            a = b;
        if (a < kLevelFloor)
            a = kLevelFloor;
        if (peak < a)
        {
            pos = i + 1;
            peak = a;
        }
    }
    return peak;
}

inline void applyGain(const float* g, const float* inL, const float* inR, float* outL, float* outR, int n) noexcept
{
    const int nv = vectorEnd(n);
    for (int i = 0; i < nv; i += 4)
    {
        const float32x4_t gv = vld1q_f32(g + i);
        vst1q_f32(outL + i, vmulq_f32(gv, vld1q_f32(inL + i)));
        vst1q_f32(outR + i, vmulq_f32(vld1q_f32(inR + i), gv));
    }
    for (int i = nv; i < n; ++i)
    {
        outL[i] = g[i] * inL[i];
        outR[i] = inR[i] * g[i];
    }
}

inline void crossfade(float* a, const float* d, const float* x, const float* c, const float* f, int n) noexcept
{
    APP_FX_NOFUSE
    const int nv = vectorEnd(n);
    for (int i = 0; i < nv; i += 4)
    {
        const float32x4_t p = vmulq_f32(vld1q_f32(d + i), vld1q_f32(c + i));
        const float32x4_t q = vmulq_f32(vld1q_f32(x + i), vld1q_f32(f + i));
        vst1q_f32(a + i, vaddq_f32(p, q));
    }
    for (int i = nv; i < n; ++i)
    {
        const float p = d[i] * c[i];
        const float q = x[i] * f[i];
        a[i] = p + q;
    }
}
} // namespace neon
#endif
} // namespace engine::fx::dsp::compressor
