#include "engine/modulation/AmpEnvelopeKernels.h"

#if APP_ENGINE_HAVE_NEON
#include <arm_neon.h>
#endif

namespace engine::modulation::envk
{
namespace scalar
{
void fillConstant(float* g, int n, float v) noexcept
{
    for (int i = 0; i < n; ++i)
        g[i] = v;
}

void fillLinear(float* g, int n, float a, float d, float k0) noexcept
{
    // Same operation order as the NEON body: one multiply, one add per sample, index exact in float (< 2^24).
    for (int i = 0; i < n; ++i)
    {
        const float prod = d * (k0 + static_cast<float>(i));
        g[i] = a + prod;
    }
}

void fillGeometric(float* g, int n, float a, const GeoSeed& s) noexcept
{
    float t0 = s.lane[0], t1 = s.lane[1], t2 = s.lane[2], t3 = s.lane[3];
    int i = 0;
    for (; i + 4 <= n; i += 4)
    {
        g[i] = a + t0;
        g[i + 1] = a + t1;
        g[i + 2] = a + t2;
        g[i + 3] = a + t3;
        t0 *= s.step;
        t1 *= s.step;
        t2 *= s.step;
        t3 *= s.step;
    }
    const float tail[3] = {t0, t1, t2};
    for (int k = 0; i < n; ++i, ++k)
        g[i] = a + tail[k];
}

void applyGain(float* l, float* r, const float* g, int n) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        l[i] *= g[i];
        r[i] *= g[i];
    }
}

void applyConstant(float* l, float* r, float v, int n) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        l[i] *= v;
        r[i] *= v;
    }
}
} // namespace scalar

#if APP_ENGINE_HAVE_NEON
namespace neon
{
void fillConstant(float* g, int n, float v) noexcept
{
    const float32x4_t vv = vdupq_n_f32(v);
    int i = 0;
    for (; i + 4 <= n; i += 4)
        vst1q_f32(g + i, vv);
    for (; i < n; ++i)
        g[i] = v;
}

void fillLinear(float* g, int n, float a, float d, float k0) noexcept
{
    const float32x4_t va = vdupq_n_f32(a), vd = vdupq_n_f32(d), four = vdupq_n_f32(4.0f);
    const float seed[4] = {k0, k0 + 1.0f, k0 + 2.0f, k0 + 3.0f};
    float32x4_t idx = vld1q_f32(seed);
    int i = 0;
    for (; i + 4 <= n; i += 4)
    {
        vst1q_f32(g + i, vaddq_f32(va, vmulq_f32(vd, idx))); // not vmla: same rounding as the scalar reference
        idx = vaddq_f32(idx, four);
    }
    for (; i < n; ++i)
    {
        const float prod = d * (k0 + static_cast<float>(i));
        g[i] = a + prod;
    }
}

void fillGeometric(float* g, int n, float a, const GeoSeed& s) noexcept
{
    const float32x4_t va = vdupq_n_f32(a), step = vdupq_n_f32(s.step);
    float32x4_t t = vld1q_f32(s.lane);
    int i = 0;
    for (; i + 4 <= n; i += 4)
    {
        vst1q_f32(g + i, vaddq_f32(va, t));
        t = vmulq_f32(t, step);
    }
    if (i < n)
    {
        float tail[4];
        vst1q_f32(tail, t);
        for (int k = 0; i < n; ++i, ++k)
            g[i] = a + tail[k];
    }
}

void applyGain(float* l, float* r, const float* g, int n) noexcept
{
    int i = 0;
    for (; i + 4 <= n; i += 4)
    {
        const float32x4_t vg = vld1q_f32(g + i);
        vst1q_f32(l + i, vmulq_f32(vld1q_f32(l + i), vg));
        vst1q_f32(r + i, vmulq_f32(vld1q_f32(r + i), vg));
    }
    for (; i < n; ++i)
    {
        l[i] *= g[i];
        r[i] *= g[i];
    }
}

void applyConstant(float* l, float* r, float v, int n) noexcept
{
    const float32x4_t vv = vdupq_n_f32(v);
    int i = 0;
    for (; i + 4 <= n; i += 4)
    {
        vst1q_f32(l + i, vmulq_f32(vld1q_f32(l + i), vv));
        vst1q_f32(r + i, vmulq_f32(vld1q_f32(r + i), vv));
    }
    for (; i < n; ++i)
    {
        l[i] *= v;
        r[i] *= v;
    }
}
} // namespace neon
#endif

namespace
{
constexpr KernelSet kScalar{&scalar::fillConstant, &scalar::fillLinear, &scalar::fillGeometric, &scalar::applyGain,
                            &scalar::applyConstant};
#if APP_ENGINE_HAVE_NEON
constexpr KernelSet kNeon{&neon::fillConstant, &neon::fillLinear, &neon::fillGeometric, &neon::applyGain,
                          &neon::applyConstant};
#endif
} // namespace

const KernelSet& scalarKernels() noexcept
{
    return kScalar;
}

const KernelSet& activeKernels() noexcept
{
#if APP_ENGINE_HAVE_NEON
    return kNeon;
#else
    return kScalar;
#endif
}
} // namespace engine::modulation::envk
