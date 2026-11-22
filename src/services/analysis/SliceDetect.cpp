#include "services/analysis/SliceDetect.h"

#include <cmath>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace services::analysis
{
namespace
{
struct Range
{
    std::int64_t lo, hi, at; // crossings considered at f ∈ [lo, hi]; at = the clamped target frame
};

Range rangeOf(const SampleBuffer& b, std::int64_t frame, int window) noexcept
{
    const std::int64_t last = b.frames - 1;
    const std::int64_t at = frame < 0 ? 0 : (frame > last ? last : frame);
    const std::int64_t w = window < 0 ? 0 : window;
    return {at - w < 1 ? 1 : at - w, at + w > last ? last : at + w, at};
}

inline float valueAt(const SampleBuffer& b, std::int64_t i) noexcept
{
    return b.channels > 1 ? b.ch[0][i] + b.ch[1][i] : b.ch[0][i];
}

// Keeps the nearest crossing (ties: the earlier, i.e. the first seen at that distance in increasing f).
inline void consider(std::int64_t f, std::int64_t at, std::int64_t& best, std::int64_t& bestDist) noexcept
{
    const std::int64_t d = f > at ? f - at : at - f;
    if (d < bestDist)
    {
        bestDist = d;
        best = f;
    }
}
} // namespace

int windowFrames(const SampleBuffer& b, double ms) noexcept
{
    const double rate = b.sampleRate > 0.0 ? b.sampleRate : 44100.0;
    return static_cast<int>(std::lround(ms * 0.001 * rate));
}

std::int64_t nearestZeroCrossingScalar(const SampleBuffer& b, std::int64_t frame, int window) noexcept
{
    if (b.frames < 2 || b.ch[0] == nullptr)
        return frame < 0 ? 0 : frame;
    const Range r = rangeOf(b, frame, window);
    std::int64_t best = r.at, bestDist = INT64_MAX;
    for (std::int64_t f = r.lo; f <= r.hi; ++f)
        if (std::signbit(valueAt(b, f - 1)) != std::signbit(valueAt(b, f)))
            consider(f, r.at, best, bestDist);
    return best;
}

#if defined(__ARM_NEON)
std::int64_t nearestZeroCrossingNeon(const SampleBuffer& b, std::int64_t frame, int window) noexcept
{
    if (b.frames < 2 || b.ch[0] == nullptr)
        return frame < 0 ? 0 : frame;
    const Range r = rangeOf(b, frame, window);
    std::int64_t best = r.at, bestDist = INT64_MAX;
    const bool stereo = b.channels > 1;
    const float* l = b.ch[0];
    const float* rr = stereo ? b.ch[1] : nullptr;
    std::int64_t f = r.lo;
    for (; f + 3 <= r.hi; f += 4) // 4 sign comparisons per step: bit 31 of (x[f−1] XOR x[f])
    {
        uint32x4_t prev = vreinterpretq_u32_f32(stereo ? vaddq_f32(vld1q_f32(l + f - 1), vld1q_f32(rr + f - 1)) : vld1q_f32(l + f - 1));
        uint32x4_t cur = vreinterpretq_u32_f32(stereo ? vaddq_f32(vld1q_f32(l + f), vld1q_f32(rr + f)) : vld1q_f32(l + f));
        const uint32x4_t x = vshrq_n_u32(veorq_u32(prev, cur), 31);
        const uint32x2_t any = vorr_u32(vget_low_u32(x), vget_high_u32(x));
        if ((vget_lane_u32(any, 0) | vget_lane_u32(any, 1)) == 0)
            continue;
        std::uint32_t lanes[4];
        vst1q_u32(lanes, x);
        for (int k = 0; k < 4; ++k)
            if (lanes[k] != 0)
                consider(f + k, r.at, best, bestDist);
    }
    for (; f <= r.hi; ++f)
        if (std::signbit(valueAt(b, f - 1)) != std::signbit(valueAt(b, f)))
            consider(f, r.at, best, bestDist);
    return best;
}
#endif

std::int64_t nearestZeroCrossing(const SampleBuffer& b, std::int64_t frame, int window) noexcept
{
#if defined(__ARM_NEON)
    return nearestZeroCrossingNeon(b, frame, window);
#else
    return nearestZeroCrossingScalar(b, frame, window);
#endif
}
} // namespace services::analysis
