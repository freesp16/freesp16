// Min/max kernels of the peak overview (REN-020), header-only so the NEON parity test (tests/neon, qemu) needs no
// services library: NEON (`__ARM_NEON`) and a scalar reference, bit-exact (min/max are exact; a final `+ 0.0f` maps
// −0 to +0 in both, so the sign of a zero never depends on lane order). No allocation; any thread.
#pragma once

#include <algorithm>
#include <cstdint>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define APP_ANALYSIS_HAVE_NEON 1
#else
#define APP_ANALYSIS_HAVE_NEON 0
#endif

namespace services::analysis::kernels
{
// mn / mx updated with x[0 … n). The caller seeds them (e.g. +inf / −inf).
inline void minMaxScalar(const float* x, int n, float& mn, float& mx) noexcept
{
    float a = mn, b = mx;
    for (int i = 0; i < n; ++i)
    {
        a = x[i] < a ? x[i] : a;
        b = x[i] > b ? x[i] : b;
    }
    mn = a + 0.0f;
    mx = b + 0.0f;
}

#if APP_ANALYSIS_HAVE_NEON
inline void minMaxNeon(const float* x, int n, float& mn, float& mx) noexcept
{
    int i = 0;
    float a = mn, b = mx;
    if (n >= 8)
    {
        float32x4_t lo0 = vdupq_n_f32(a), lo1 = lo0, hi0 = vdupq_n_f32(b), hi1 = hi0;
        for (; i + 8 <= n; i += 8)
        {
            const float32x4_t v0 = vld1q_f32(x + i), v1 = vld1q_f32(x + i + 4);
            lo0 = vminq_f32(lo0, v0);
            lo1 = vminq_f32(lo1, v1);
            hi0 = vmaxq_f32(hi0, v0);
            hi1 = vmaxq_f32(hi1, v1);
        }
        const float32x4_t lo = vminq_f32(lo0, lo1), hi = vmaxq_f32(hi0, hi1);
        float32x2_t l2 = vpmin_f32(vget_low_f32(lo), vget_high_f32(lo));
        float32x2_t h2 = vpmax_f32(vget_low_f32(hi), vget_high_f32(hi));
        l2 = vpmin_f32(l2, l2);
        h2 = vpmax_f32(h2, h2);
        a = vget_lane_f32(l2, 0);
        b = vget_lane_f32(h2, 0);
    }
    for (; i < n; ++i)
    {
        a = x[i] < a ? x[i] : a;
        b = x[i] > b ? x[i] : b;
    }
    mn = a + 0.0f;
    mx = b + 0.0f;
}
#endif

inline void minMax(const float* x, int n, float& mn, float& mx) noexcept
{
#if APP_ANALYSIS_HAVE_NEON
    minMaxNeon(x, n, mn, mx);
#else
    minMaxScalar(x, n, mn, mx);
#endif
}
} // namespace services::analysis::kernels
