#include "engine/fx/dsp/Vec.h"

#if APP_FX_HAVE_NEON
#include <arm_neon.h>
#endif

namespace engine::fx::dsp::vec
{
namespace scalar
{
void copy(float* dst, const float* src, int n) noexcept
{
    if (n > 0)
        std::memmove(dst, src, sizeof(float) * static_cast<std::size_t>(n));
}

void mul(float* x, float g, int n) noexcept
{
    for (int i = 0; i < n; ++i)
        x[i] *= g;
}

void mulv(float* x, const float* g, int n) noexcept
{
    for (int i = 0; i < n; ++i)
        x[i] *= g[i];
}

void add(float* dst, const float* src, int n) noexcept
{
    for (int i = 0; i < n; ++i)
        dst[i] += src[i];
}

void mix(float* x, const float* c, float d, float w, int n) noexcept
{
    APP_FX_NOFUSE
    for (int i = 0; i < n; ++i)
    {
        const float a = x[i] * d;
        const float b = c[i] * w;
        x[i] = a + b;
    }
}

void mixv(float* x, const float* c, const float* d, const float* w, int n) noexcept
{
    APP_FX_NOFUSE
    for (int i = 0; i < n; ++i)
    {
        const float a = x[i] * d[i];
        const float b = c[i] * w[i];
        x[i] = a + b;
    }
}

float peakAbs(const float* x, int n) noexcept
{
    float p = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float a = absf(x[i]);
        p = a > p ? a : p;
    }
    return p;
}
} // namespace scalar

#if APP_FX_HAVE_NEON
namespace neon
{
void copy(float* dst, const float* src, int n) noexcept
{
    int i = 0;
    for (; i + 4 <= n; i += 4)
        vst1q_f32(dst + i, vld1q_f32(src + i));
    for (; i < n; ++i)
        dst[i] = src[i];
}

void mul(float* x, float g, int n) noexcept
{
    int i = 0;
    for (; i + 4 <= n; i += 4)
        vst1q_f32(x + i, vmulq_n_f32(vld1q_f32(x + i), g));
    for (; i < n; ++i)
        x[i] *= g;
}

void mulv(float* x, const float* g, int n) noexcept
{
    int i = 0;
    for (; i + 4 <= n; i += 4)
        vst1q_f32(x + i, vmulq_f32(vld1q_f32(x + i), vld1q_f32(g + i)));
    for (; i < n; ++i)
        x[i] *= g[i];
}

void add(float* dst, const float* src, int n) noexcept
{
    int i = 0;
    for (; i + 4 <= n; i += 4)
        vst1q_f32(dst + i, vaddq_f32(vld1q_f32(dst + i), vld1q_f32(src + i)));
    for (; i < n; ++i)
        dst[i] += src[i];
}

void mix(float* x, const float* c, float d, float w, int n) noexcept
{
    APP_FX_NOFUSE
    int i = 0;
    for (; i + 4 <= n; i += 4)
    {
        const float32x4_t a = vmulq_n_f32(vld1q_f32(x + i), d);
        const float32x4_t b = vmulq_n_f32(vld1q_f32(c + i), w);
        vst1q_f32(x + i, vaddq_f32(a, b));
    }
    for (; i < n; ++i)
    {
        const float a = x[i] * d;
        const float b = c[i] * w;
        x[i] = a + b;
    }
}

void mixv(float* x, const float* c, const float* d, const float* w, int n) noexcept
{
    APP_FX_NOFUSE
    int i = 0;
    for (; i + 4 <= n; i += 4)
    {
        const float32x4_t a = vmulq_f32(vld1q_f32(x + i), vld1q_f32(d + i));
        const float32x4_t b = vmulq_f32(vld1q_f32(c + i), vld1q_f32(w + i));
        vst1q_f32(x + i, vaddq_f32(a, b));
    }
    for (; i < n; ++i)
    {
        const float a = x[i] * d[i];
        const float b = c[i] * w[i];
        x[i] = a + b;
    }
}

float peakAbs(const float* x, int n) noexcept
{
    int i = 0;
    float32x4_t m = vdupq_n_f32(0.0f);
    for (; i + 4 <= n; i += 4)
        m = vmaxq_f32(m, vabsq_f32(vld1q_f32(x + i)));
    const float32x2_t h = vpmax_f32(vget_low_f32(m), vget_high_f32(m));
    float p = vget_lane_f32(vpmax_f32(h, h), 0);
    for (; i < n; ++i)
    {
        const float a = absf(x[i]);
        p = a > p ? a : p;
    }
    return p;
}
} // namespace neon
#endif
} // namespace engine::fx::dsp::vec
