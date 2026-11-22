#include "engine/sampling/Threshold.h"

#include <array>

#if APP_SAMPLING_HAVE_NEON
 #include <arm_neon.h>
#endif

namespace engine::sampling
{
namespace
{
// 10^(dB / 20) for dB = −60 … −20, built at compile time (no libm on the audio thread): −60 dB = 0.001 exactly, each
// further step × 10^(1/20).
constexpr std::array<float, 41> makeGains() noexcept
{
    std::array<float, 41> g {};
    constexpr double step = 1.1220184543019633; // 10^(1/20)
    double v = 0.001;
    for (int i = 0; i < 41; ++i)
    {
        g[static_cast<std::size_t>(i)] = static_cast<float>(v);
        v *= step;
    }
    return g;
}
constexpr auto kGains = makeGains();

inline float absf(float x) noexcept { return x < 0.0f ? -x : x; }
} // namespace

float thresholdGain(int index) noexcept
{
    return index >= 1 && index <= 41 ? kGains[static_cast<std::size_t>(index - 1)] : 0.0f;
}

int scalar::firstAbove(const float* l, const float* r, int n, float thr) noexcept
{
    if (r == nullptr)
        r = l;
    for (int i = 0; i < n; ++i)
        if (absf(l[i]) > thr || absf(r[i]) > thr)
            return i;
    return -1;
}

#if APP_SAMPLING_HAVE_NEON
int neon::firstAbove(const float* l, const float* r, int n, float thr) noexcept
{
    if (r == nullptr)
        r = l;
    const float32x4_t t = vdupq_n_f32(thr);
    const int body = n & ~3;
    for (int i = 0; i < body; i += 4)
    {
        const uint32x4_t over = vorrq_u32(vcagtq_f32(vld1q_f32(l + i), t), vcagtq_f32(vld1q_f32(r + i), t));
        const uint32x2_t any = vorr_u32(vget_low_u32(over), vget_high_u32(over));
        if ((vget_lane_u32(any, 0) | vget_lane_u32(any, 1)) == 0)
            continue;
        for (int k = 0; k < 4; ++k) // the first set lane (the scalar compare decides: identical results)
            if (absf(l[i + k]) > thr || absf(r[i + k]) > thr)
                return i + k;
    }
    for (int i = body; i < n; ++i)
        if (absf(l[i]) > thr || absf(r[i]) > thr)
            return i;
    return -1;
}
#endif
} // namespace engine::sampling
