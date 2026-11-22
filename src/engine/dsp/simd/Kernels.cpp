#include "engine/dsp/simd/Kernels.h"

#if APP_ENGINE_HAVE_NEON
 #include <arm_neon.h>
#endif

namespace engine::simd
{
namespace scalar
{
PeakSum peakSum(const float* x, int n) noexcept
{
    float peak = 0.0f, sum = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float a = x[i] < 0.0f ? -x[i] : x[i];
        peak = a > peak ? a : peak;
        sum += x[i] * x[i];
    }
    return {peak, sum};
}

float peakAbs(const float* x, int n) noexcept
{
    float peak = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float a = x[i] < 0.0f ? -x[i] : x[i];
        peak = a > peak ? a : peak;
    }
    return peak;
}

void add(float* dst, const float* src, int n) noexcept
{
    for (int i = 0; i < n; ++i)
        dst[i] += src[i];
}

void scale(float* dst, const float* src, float g, int n) noexcept
{
    for (int i = 0; i < n; ++i)
        dst[i] = src[i] * g;
}

void mulAdd(float* dst, const float* src, const float* g, int n) noexcept
{
    for (int i = 0; i < n; ++i)
        dst[i] += src[i] * g[i];
}

void multiply(float* dst, const float* g, int n) noexcept
{
    for (int i = 0; i < n; ++i)
        dst[i] *= g[i];
}
} // namespace scalar

#if APP_ENGINE_HAVE_NEON
namespace neon
{
// ARMv7 subset only (no vmaxvq/vaddvq: those are AArch64): horizontal reductions with vpmax/vpadd on 2-lane halves.
namespace
{
inline float hmax(float32x4_t v) noexcept
{
    float32x2_t m = vpmax_f32(vget_low_f32(v), vget_high_f32(v));
    m = vpmax_f32(m, m);
    return vget_lane_f32(m, 0);
}
inline float hsum(float32x4_t v) noexcept
{
    float32x2_t s = vadd_f32(vget_low_f32(v), vget_high_f32(v));
    s = vpadd_f32(s, s);
    return vget_lane_f32(s, 0);
}
} // namespace

PeakSum peakSum(const float* x, int n) noexcept
{
    float32x4_t peak0 = vdupq_n_f32(0.0f), peak1 = peak0, sum0 = peak0, sum1 = peak0;
    int i = 0;
    for (; i + 8 <= n; i += 8) // two accumulators: hides the A9's 4-cycle VMLA latency
    {
        const float32x4_t a = vld1q_f32(x + i), b = vld1q_f32(x + i + 4);
        peak0 = vmaxq_f32(peak0, vabsq_f32(a));
        peak1 = vmaxq_f32(peak1, vabsq_f32(b));
        sum0 = vmlaq_f32(sum0, a, a);
        sum1 = vmlaq_f32(sum1, b, b);
    }
    for (; i + 4 <= n; i += 4)
    {
        const float32x4_t a = vld1q_f32(x + i);
        peak0 = vmaxq_f32(peak0, vabsq_f32(a));
        sum0 = vmlaq_f32(sum0, a, a);
    }
    PeakSum r {hmax(vmaxq_f32(peak0, peak1)), hsum(vaddq_f32(sum0, sum1))};
    const PeakSum tail = scalar::peakSum(x + i, n - i);
    r.peak = tail.peak > r.peak ? tail.peak : r.peak;
    r.sumSquares += tail.sumSquares;
    return r;
}

float peakAbs(const float* x, int n) noexcept
{
    float32x4_t peak0 = vdupq_n_f32(0.0f), peak1 = peak0;
    int i = 0;
    for (; i + 8 <= n; i += 8)
    {
        peak0 = vmaxq_f32(peak0, vabsq_f32(vld1q_f32(x + i)));
        peak1 = vmaxq_f32(peak1, vabsq_f32(vld1q_f32(x + i + 4)));
    }
    for (; i + 4 <= n; i += 4)
        peak0 = vmaxq_f32(peak0, vabsq_f32(vld1q_f32(x + i)));
    const float p = hmax(vmaxq_f32(peak0, peak1));
    const float t = scalar::peakAbs(x + i, n - i);
    return t > p ? t : p;
}

void add(float* dst, const float* src, int n) noexcept
{
    int i = 0;
    for (; i + 4 <= n; i += 4)
        vst1q_f32(dst + i, vaddq_f32(vld1q_f32(dst + i), vld1q_f32(src + i)));
    scalar::add(dst + i, src + i, n - i);
}

void scale(float* dst, const float* src, float g, int n) noexcept
{
    int i = 0;
    for (; i + 4 <= n; i += 4)
        vst1q_f32(dst + i, vmulq_n_f32(vld1q_f32(src + i), g));
    scalar::scale(dst + i, src + i, g, n - i);
}

void mulAdd(float* dst, const float* src, const float* g, int n) noexcept
{
    // vmlaq is a fused-free multiply-accumulate on ARMv7 (separate rounding), identical to the scalar reference
    // when the compiler does not contract the scalar loop; tests compare with a 1-ulp tolerance either way.
    int i = 0;
    for (; i + 4 <= n; i += 4)
        vst1q_f32(dst + i, vmlaq_f32(vld1q_f32(dst + i), vld1q_f32(src + i), vld1q_f32(g + i)));
    scalar::mulAdd(dst + i, src + i, g + i, n - i);
}

void multiply(float* dst, const float* g, int n) noexcept
{
    int i = 0;
    for (; i + 4 <= n; i += 4)
        vst1q_f32(dst + i, vmulq_f32(vld1q_f32(dst + i), vld1q_f32(g + i)));
    scalar::multiply(dst + i, g + i, n - i);
}
} // namespace neon
#endif
} // namespace engine::simd
