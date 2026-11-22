#include "engine/mixer/OutputRouterKernels.h"

#if APP_ROUTING_HAVE_NEON
 #include <arm_neon.h>
#endif

// One rounding per operation, NEON = scalar bit for bit (OutputRouterKernels.h).
#if defined(__clang__)
 #pragma clang fp contract(off)
#elif defined(__GNUC__)
 #pragma GCC optimize("fp-contract=off")
#endif

namespace engine::mixer::routing
{
namespace scalar
{
void add(float* dst, const float* src, int n) noexcept
{
    for (int i = 0; i < n; ++i)
        dst[i] = dst[i] + src[i];
}
void addGain(float* dst, const float* src, const float* g, int n) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const float x = src[i] * g[i];
        dst[i] = dst[i] + x;
    }
}
void addMono(float* dst, const float* l, const float* r, int n) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const float s = l[i] + r[i];
        const float m = s * 0.5f;
        dst[i] = dst[i] + m;
    }
}
void addMonoGain(float* dst, const float* l, const float* r, const float* g, int n) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const float s = l[i] + r[i];
        const float m = s * 0.5f;
        const float x = m * g[i];
        dst[i] = dst[i] + x;
    }
}
void copyGain(float* dst, const float* src, const float* g, int n) noexcept
{
    for (int i = 0; i < n; ++i)
        dst[i] = src[i] * g[i];
}
void copyScale(float* dst, const float* src, float g, int n) noexcept
{
    for (int i = 0; i < n; ++i)
        dst[i] = src[i] * g;
}
void multiply(float* dst, const float* g, int n) noexcept
{
    for (int i = 0; i < n; ++i)
        dst[i] = dst[i] * g[i];
}
} // namespace scalar

#if APP_ROUTING_HAVE_NEON
namespace neon
{
void add(float* dst, const float* src, int n) noexcept
{
    for (int i = 0; i < n; i += 4)
        vst1q_f32(dst + i, vaddq_f32(vld1q_f32(dst + i), vld1q_f32(src + i)));
}
void addGain(float* dst, const float* src, const float* g, int n) noexcept
{
    for (int i = 0; i < n; i += 4)
    {
        const float32x4_t x = vmulq_f32(vld1q_f32(src + i), vld1q_f32(g + i));
        vst1q_f32(dst + i, vaddq_f32(vld1q_f32(dst + i), x));
    }
}
void addMono(float* dst, const float* l, const float* r, int n) noexcept
{
    const float32x4_t half = vdupq_n_f32(0.5f);
    for (int i = 0; i < n; i += 4)
    {
        const float32x4_t m = vmulq_f32(vaddq_f32(vld1q_f32(l + i), vld1q_f32(r + i)), half);
        vst1q_f32(dst + i, vaddq_f32(vld1q_f32(dst + i), m));
    }
}
void addMonoGain(float* dst, const float* l, const float* r, const float* g, int n) noexcept
{
    const float32x4_t half = vdupq_n_f32(0.5f);
    for (int i = 0; i < n; i += 4)
    {
        const float32x4_t m = vmulq_f32(vaddq_f32(vld1q_f32(l + i), vld1q_f32(r + i)), half);
        const float32x4_t x = vmulq_f32(m, vld1q_f32(g + i));
        vst1q_f32(dst + i, vaddq_f32(vld1q_f32(dst + i), x));
    }
}
void copyGain(float* dst, const float* src, const float* g, int n) noexcept
{
    for (int i = 0; i < n; i += 4)
        vst1q_f32(dst + i, vmulq_f32(vld1q_f32(src + i), vld1q_f32(g + i)));
}
void copyScale(float* dst, const float* src, float g, int n) noexcept
{
    const float32x4_t v = vdupq_n_f32(g);
    for (int i = 0; i < n; i += 4)
        vst1q_f32(dst + i, vmulq_f32(vld1q_f32(src + i), v));
}
void multiply(float* dst, const float* g, int n) noexcept
{
    for (int i = 0; i < n; i += 4)
        vst1q_f32(dst + i, vmulq_f32(vld1q_f32(dst + i), vld1q_f32(g + i)));
}
} // namespace neon
#endif

void SelectorFade::fill(float* out, int n) noexcept
{
    int i = 0;
    if (count_ > 0)
    {
        const int k = count_ < n ? count_ : n;
        float g = cur_;
        for (; i < k; ++i)
        {
            if (out != nullptr)
                out[i] = g;
            g = g + step_;
        }
        if (count_ > k)
        {
            count_ -= k;
            cur_ = g;
            return;
        }
        cur_ = target_;
        count_ = 0;
        step_ = 0.0f;
    }
    if (out != nullptr)
        for (; i < n; ++i)
            out[i] = cur_;
}
} // namespace engine::mixer::routing
