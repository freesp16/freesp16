#include "engine/voice/InterpKernels.h"

#include <cmath>
#include <cstring>

#if APP_ENGINE_HAVE_NEON
 #include <arm_neon.h>
#endif

namespace engine::voice::kernels
{
namespace scalar
{
void hermite4(const float* const* src, int channels, Fixed pos, Fixed step, float* const* dst, int frames) noexcept
{
    for (int c = 0; c < channels; ++c)
    {
        const float* s = src[c];
        float* d = dst[c];
        Fixed p = pos;
        for (int k = 0; k < frames; ++k, p += step)
        {
            const float* t = s + indexOf(p) - 1;
            const HermiteWeights w = hermiteWeights(fractionOf(p));
            d[k] = t[0] * w.wm1 + t[1] * w.w0 + t[2] * w.w1 + t[3] * w.w2;
        }
    }
}

void linear(const float* const* src, int channels, Fixed pos, Fixed step, float* const* dst, int frames) noexcept
{
    for (int c = 0; c < channels; ++c)
    {
        const float* s = src[c];
        float* d = dst[c];
        Fixed p = pos;
        for (int k = 0; k < frames; ++k, p += step)
        {
            const float* t = s + indexOf(p);
            const float x = fractionOf(p);
            d[k] = t[0] + x * (t[1] - t[0]);
        }
    }
}
} // namespace scalar

#if APP_ENGINE_HAVE_NEON
namespace neon
{
namespace
{
// One channel's 4 output frames: rows a…d are the taps of frames 0…3; transposed, row m holds tap m of every frame.
inline float32x4_t hermiteGroup(const float* s, std::int64_t i0, std::int64_t i1, std::int64_t i2, std::int64_t i3,
                                float32x4_t wm1, float32x4_t w0, float32x4_t w1, float32x4_t w2) noexcept
{
    const float32x4_t a = vld1q_f32(s + i0 - 1);
    const float32x4_t b = vld1q_f32(s + i1 - 1);
    const float32x4_t c = vld1q_f32(s + i2 - 1);
    const float32x4_t d = vld1q_f32(s + i3 - 1);
    const float32x4x2_t ab = vtrnq_f32(a, b); // [a0 b0 a2 b2] [a1 b1 a3 b3]
    const float32x4x2_t cd = vtrnq_f32(c, d);
    const float32x4_t t0 = vcombine_f32(vget_low_f32(ab.val[0]), vget_low_f32(cd.val[0]));
    const float32x4_t t1 = vcombine_f32(vget_low_f32(ab.val[1]), vget_low_f32(cd.val[1]));
    const float32x4_t t2 = vcombine_f32(vget_high_f32(ab.val[0]), vget_high_f32(cd.val[0]));
    const float32x4_t t3 = vcombine_f32(vget_high_f32(ab.val[1]), vget_high_f32(cd.val[1]));
    float32x4_t out = vmulq_f32(t0, wm1);
    out = vmlaq_f32(out, t1, w0);
    out = vmlaq_f32(out, t2, w1);
    return vmlaq_f32(out, t3, w2);
}

template <int Channels>
void hermiteBody(const float* const* src, Fixed& pos, Fixed step, float* const* dst, int groups) noexcept
{
    const std::uint32_t l0 = static_cast<std::uint32_t>(pos), ls = static_cast<std::uint32_t>(step);
    const std::uint32_t init[4] = {l0, l0 + ls, l0 + 2u * ls, l0 + 3u * ls};
    uint32x4_t lows = vld1q_u32(init);
    const uint32x4_t inc = vdupq_n_u32(4u * ls);
    const float32x4_t half = vdupq_n_f32(0.5f), one = vdupq_n_f32(1.0f), two = vdupq_n_f32(2.0f);
    const float32x4_t oneHalf = vdupq_n_f32(1.5f), twoHalf = vdupq_n_f32(2.5f);
    const float* s0 = src[0];
    const float* s1 = Channels == 2 ? src[1] : nullptr;
    float* d0 = dst[0];
    float* d1 = Channels == 2 ? dst[1] : nullptr;
    Fixed p = pos;
    for (int g = 0; g < groups; ++g)
    {
        const float32x4_t x = vcvtq_n_f32_u32(lows, 32); // low word / 2^32: the fraction
        lows = vaddq_u32(lows, inc);
        const float32x4_t x2 = vmulq_f32(x, x);
        // wm1 = ((−x/2 + 1)x − 1/2)x   w0 = (3x/2 − 5/2)x² + 1   w1 = ((−3x/2 + 2)x + 1/2)x   w2 = (x/2 − 1/2)x²
        const float32x4_t wm1 = vmulq_f32(vsubq_f32(vmulq_f32(vmlsq_f32(one, half, x), x), half), x);
        const float32x4_t w0 = vmlaq_f32(one, vmlaq_f32(vnegq_f32(twoHalf), oneHalf, x), x2);
        const float32x4_t w1 = vmulq_f32(vmlaq_f32(half, vmlsq_f32(two, oneHalf, x), x), x);
        const float32x4_t w2 = vmulq_f32(vmlsq_f32(vnegq_f32(half), vnegq_f32(half), x), x2);
        const std::int64_t i0 = indexOf(p);
        const std::int64_t i1 = indexOf(p + step);
        const std::int64_t i2 = indexOf(p + 2 * step);
        const std::int64_t i3 = indexOf(p + 3 * step);
        p += 4 * step;
        vst1q_f32(d0 + 4 * g, hermiteGroup(s0, i0, i1, i2, i3, wm1, w0, w1, w2));
        if constexpr (Channels == 2)
            vst1q_f32(d1 + 4 * g, hermiteGroup(s1, i0, i1, i2, i3, wm1, w0, w1, w2));
    }
    pos = p;
}
} // namespace

void hermite4(const float* const* src, int channels, Fixed pos, Fixed step, float* const* dst, int frames) noexcept
{
    const int groups = frames / 4;
    Fixed p = pos;
    if (channels == 2)
        hermiteBody<2>(src, p, step, dst, groups);
    else
        hermiteBody<1>(src, p, step, dst, groups);
    const int done = groups * 4;
    if (done < frames)
    {
        float* tail[2] = {dst[0] + done, channels == 2 ? dst[1] + done : nullptr};
        scalar::hermite4(src, channels, p, step, tail, frames - done);
    }
}
} // namespace neon
#endif

void copy(const float* const* src, int channels, std::int64_t index, int direction, float* const* dst,
          int frames) noexcept
{
    if (frames <= 0)
        return;
    for (int c = 0; c < channels; ++c)
    {
        const float* s = src[c] + index;
        float* d = dst[c];
        if (direction > 0)
        {
            std::memcpy(d, s, static_cast<std::size_t>(frames) * sizeof(float));
            continue;
        }
        int k = 0;
#if APP_ENGINE_HAVE_NEON
        for (; k + 4 <= frames; k += 4)
        {
            const float32x4_t v = vrev64q_f32(vld1q_f32(s - k - 3)); // [s−k−2, s−k−3, s−k, s−k−1]
            vst1q_f32(d + k, vcombine_f32(vget_high_f32(v), vget_low_f32(v)));
        }
#endif
        for (; k < frames; ++k)
            d[k] = s[-k];
    }
}

namespace
{
// Splits [0, frames) into runs whose taps idx + lo … idx + hi all lie in the readable range (the fast kernel) and
// single frames that need the zero-outside reader.
template <typename Fast, typename Slow>
void splitRuns(std::int64_t srcFrames, int pad, int lo, int hi, Fixed pos, Fixed step, int frames, Fast&& fast,
               Slow&& slow) noexcept
{
    const std::int64_t first = -static_cast<std::int64_t>(pad) - lo;   // smallest idx with idx + lo >= −pad
    const std::int64_t last = srcFrames + pad - 1 - hi;                // largest idx with idx + hi < srcFrames + pad
    int k = 0;
    Fixed p = pos;
    while (k < frames)
    {
        int m = 0;
        Fixed q = p;
        while (k + m < frames)
        {
            const std::int64_t i = indexOf(q);
            if (i < first || i > last)
                break;
            ++m;
            q += step;
        }
        if (m > 0)
        {
            fast(k, p, m);
            k += m;
            p = q;
        }
        else
        {
            slow(k, p);
            ++k;
            p += step;
        }
    }
}

inline float tapAt(const float* s, std::int64_t i, std::int64_t srcFrames, int pad) noexcept
{
    return i >= -pad && i < srcFrames + pad ? s[i] : 0.0f;
}
} // namespace

void hermite4Checked(const float* const* src, int channels, std::int64_t srcFrames, int pad, Fixed pos, Fixed step,
                     float* const* dst, int frames) noexcept
{
    splitRuns(
        srcFrames, pad, -1, 2, pos, step, frames,
        [&](int k, Fixed p, int m) {
            float* d[2] = {dst[0] + k, channels == 2 ? dst[1] + k : nullptr};
            hermite4(src, channels, p, step, d, m);
        },
        [&](int k, Fixed p) {
            const std::int64_t i = indexOf(p);
            const HermiteWeights w = hermiteWeights(fractionOf(p));
            for (int c = 0; c < channels; ++c)
                dst[c][k] = tapAt(src[c], i - 1, srcFrames, pad) * w.wm1 + tapAt(src[c], i, srcFrames, pad) * w.w0
                    + tapAt(src[c], i + 1, srcFrames, pad) * w.w1 + tapAt(src[c], i + 2, srcFrames, pad) * w.w2;
        });
}

void linearChecked(const float* const* src, int channels, std::int64_t srcFrames, int pad, Fixed pos, Fixed step,
                   float* const* dst, int frames) noexcept
{
    splitRuns(
        srcFrames, pad, 0, 1, pos, step, frames,
        [&](int k, Fixed p, int m) {
            float* d[2] = {dst[0] + k, channels == 2 ? dst[1] + k : nullptr};
            scalar::linear(src, channels, p, step, d, m);
        },
        [&](int k, Fixed p) {
            const std::int64_t i = indexOf(p);
            const float x = fractionOf(p);
            for (int c = 0; c < channels; ++c)
            {
                const float a = tapAt(src[c], i, srcFrames, pad), b = tapAt(src[c], i + 1, srcFrames, pad);
                dst[c][k] = a + x * (b - a);
            }
        });
}
namespace
{
// Windowed-sinc tables, one per cutoff band, built once at start-up (static initialisation on the main thread, long
// before the audio thread runs; 5 × 257 × 16 floats = 82 KB). Band b serves |rate| <= kBandRate[b] with cutoff
// 0.92 / kBandRate[b] of Nyquist; faster rates use the last band.
constexpr int kSincPhases = 256;
constexpr int kSincBands = 5;
constexpr double kBandRate[kSincBands] = {1.0, 1.4142135623730951, 2.0, 2.8284271247461903, 4.0};

struct SincTables
{
    alignas(16) float coef[kSincBands][kSincPhases + 1][kSincTaps];

    static double besselI0(double x) noexcept
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 40; ++k)
        {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
        }
        return sum;
    }

    SincTables() noexcept
    {
        constexpr double beta = 7.0, half = kSincTaps / 2.0, pi = 3.14159265358979323846;
        const double i0beta = besselI0(beta);
        for (int b = 0; b < kSincBands; ++b)
        {
            const double fc = 0.92 / kBandRate[b];
            for (int ph = 0; ph <= kSincPhases; ++ph)
            {
                const double x = static_cast<double>(ph) / kSincPhases;
                double h[kSincTaps], sum = 0.0;
                for (int j = 0; j < kSincTaps; ++j)
                {
                    const double t = static_cast<double>(j - (kSincTaps / 2 - 1)) - x; // tap idx − 7 + j
                    const double u = fc * t;
                    const double sinc = std::fabs(u) < 1e-12 ? 1.0 : std::sin(pi * u) / (pi * u);
                    const double r = t / half;
                    const double w = r * r < 1.0 ? besselI0(beta * std::sqrt(1.0 - r * r)) / i0beta : 0.0;
                    h[j] = fc * sinc * w;
                    sum += h[j];
                }
                for (int j = 0; j < kSincTaps; ++j)
                    coef[b][ph][j] = static_cast<float>(h[j] / sum); // unity gain at DC for every phase
            }
        }
    }
};
const SincTables gSinc;

int sincBand(Fixed step) noexcept
{
    const double rate = std::fabs(fromFixed(step));
    for (int b = 0; b < kSincBands; ++b)
        if (rate <= kBandRate[b] * 1.000001)
            return b;
    return kSincBands - 1;
}

// One output sample: taps t[0 … 15] = src[idx − 7 … idx + 8], coefficients interpolated between two phases.
inline float sincDot(const float* t, const float* c0, const float* c1, float f) noexcept
{
    float acc = 0.0f;
    for (int j = 0; j < kSincTaps; ++j)
        acc += t[j] * (c0[j] + f * (c1[j] - c0[j]));
    return acc;
}
} // namespace

void sinc16Checked(const float* const* src, int channels, std::int64_t srcFrames, int pad, Fixed pos, Fixed step,
                   float* const* dst, int frames) noexcept
{
    const auto& table = gSinc.coef[sincBand(step)];
    constexpr int lo = -(kSincTaps / 2 - 1), hi = kSincTaps / 2;
    const auto phaseOf = [](Fixed p, int& ph, float& f) noexcept {
        const std::uint32_t low = static_cast<std::uint32_t>(p);
        ph = static_cast<int>(low >> 24); // 256 phases = the top 8 bits of the fraction
        f = static_cast<float>(low & 0xFFFFFFu) * (1.0f / 16777216.0f);
    };
    splitRuns(
        srcFrames, pad, lo, hi, pos, step, frames,
        [&](int k, Fixed p, int m) {
            for (int i = 0; i < m; ++i, p += step)
            {
                int ph;
                float f;
                phaseOf(p, ph, f);
                const std::int64_t idx = indexOf(p);
                for (int c = 0; c < channels; ++c)
                    dst[c][k + i] = sincDot(src[c] + idx + lo, table[ph], table[ph + 1], f);
            }
        },
        [&](int k, Fixed p) {
            int ph;
            float f;
            phaseOf(p, ph, f);
            const std::int64_t idx = indexOf(p);
            for (int c = 0; c < channels; ++c)
            {
                float t[kSincTaps];
                for (int j = 0; j < kSincTaps; ++j)
                    t[j] = tapAt(src[c], idx + lo + j, srcFrames, pad);
                dst[c][k] = sincDot(t, table[ph], table[ph + 1], f);
            }
        });
}
} // namespace engine::voice::kernels
