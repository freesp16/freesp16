#include "services/library/Resampler.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace services::library
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

double besselI0(double x) noexcept
{
    double sum = 1, term = 1;
    const double q = x * x / 4;
    for (int k = 1; k < 64; ++k)
    {
        term *= q / (static_cast<double>(k) * k);
        sum += term;
        if (term < sum * 1e-17)
            break;
    }
    return sum;
}

float dotScalar(const float* x, const float* c, int n) noexcept
{
    float a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    for (int j = 0; j < n; j += 4)
    {
        a0 += x[j] * c[j];
        a1 += x[j + 1] * c[j + 1];
        a2 += x[j + 2] * c[j + 2];
        a3 += x[j + 3] * c[j + 3];
    }
    return (a0 + a1) + (a2 + a3);
}

#if defined(__ARM_NEON)
float dotNeon(const float* x, const float* c, int n) noexcept
{
    float32x4_t a = vdupq_n_f32(0.0f), b = vdupq_n_f32(0.0f);
    int j = 0;
    for (; j + 8 <= n; j += 8)
    {
        a = vmlaq_f32(a, vld1q_f32(x + j), vld1q_f32(c + j));
        b = vmlaq_f32(b, vld1q_f32(x + j + 4), vld1q_f32(c + j + 4));
    }
    if (j < n) // n % 8 == 4
        a = vmlaq_f32(a, vld1q_f32(x + j), vld1q_f32(c + j));
    a = vaddq_f32(a, b);
    float32x2_t s = vadd_f32(vget_low_f32(a), vget_high_f32(a));
    s = vpadd_f32(s, s);
    return vget_lane_f32(s, 0);
}
#endif

// Dot product with the input read as zero outside [0, frames): the edges of the sample.
float dotEdge(const float* in, std::int64_t frames, std::int64_t start, const float* c, int n) noexcept
{
    float acc = 0;
    for (int j = 0; j < n; ++j)
    {
        const std::int64_t i = start + j;
        if (i >= 0 && i < frames)
            acc += in[i] * c[j];
    }
    return acc;
}

bool whole(double v) noexcept
{
    return v > 0 && v < 1e7 && std::floor(v) == v;
}
} // namespace

bool Resampler::neonAvailable() noexcept
{
#if defined(__ARM_NEON)
    return true;
#else
    return false;
#endif
}

Resampler::Resampler(double inputRate, double outputRate, int baseTaps, double beta, double cutoff)
    : in_(inputRate), out_(outputRate)
{
    const double down = std::min(1.0, out_ / in_); // < 1 when downsampling
    taps_ = std::clamp(static_cast<int>(std::ceil(baseTaps / down / 4.0)) * 4, 8, 256);
    const double fc = cutoff * down; // cycles per input sample × 2 (1 = input Nyquist)

    if (whole(in_) && whole(out_))
    {
        const auto a = static_cast<std::int64_t>(in_), b = static_cast<std::int64_t>(out_);
        const std::int64_t g = std::gcd(a, b);
        if (b / g <= kMaxPhases)
        {
            exact_ = true;
            phases_ = static_cast<int>(b / g);
            step_ = a / g;
        }
    }
    if (! exact_)
    {
        phases_ = kMaxPhases;
        ratio_ = in_ / out_;
    }

    const int half = taps_ / 2;
    const double i0b = besselI0(beta);
    table_.assign(static_cast<std::size_t>(phases_ + 1) * static_cast<std::size_t>(taps_), 0.0f);
    std::vector<double> row(static_cast<std::size_t>(taps_));
    for (int k = 0; k <= phases_; ++k)
    {
        const double frac = static_cast<double>(k) / phases_;
        double sum = 0;
        for (int j = 0; j < taps_; ++j)
        {
            const double x = (j - (half - 1)) - frac; // distance from the output instant, input frames
            const double u = x / half;
            const double w = std::fabs(u) >= 1 ? 0.0 : besselI0(beta * std::sqrt(1 - u * u)) / i0b;
            const double y = fc * x;
            const double sinc = std::fabs(y) < 1e-12 ? 1.0 : std::sin(kPi * y) / (kPi * y);
            row[static_cast<std::size_t>(j)] = fc * sinc * w;
            sum += row[static_cast<std::size_t>(j)];
        }
        float* dst = table_.data() + static_cast<std::size_t>(k) * static_cast<std::size_t>(taps_);
        for (int j = 0; j < taps_; ++j)
            dst[j] = static_cast<float>(row[static_cast<std::size_t>(j)] / sum); // unity DC gain per phase
    }
}

std::int64_t Resampler::outputFrames(std::int64_t inFrames) const noexcept
{
    if (inFrames <= 0)
        return 0;
    if (exact_)
        return (inFrames * phases_ + step_ - 1) / step_;
    return static_cast<std::int64_t>(std::ceil(static_cast<double>(inFrames) * out_ / in_));
}

void Resampler::process(const float* in, std::int64_t inFrames, float* out, std::int64_t outFrames,
                        Kernel kernel) const noexcept
{
    const bool neon = kernel != Kernel::Scalar && neonAvailable();
    auto dot = [neon](const float* x, const float* c, int n) noexcept
    {
#if defined(__ARM_NEON)
        if (neon)
            return dotNeon(x, c, n);
#else
        (void)neon;
#endif
        return dotScalar(x, c, n);
    };
    const int n = taps_;
    const int lead = n / 2 - 1;
    if (exact_)
    {
        std::int64_t i0 = 0, k = 0;
        for (std::int64_t o = 0; o < outFrames; ++o)
        {
            const std::int64_t start = i0 - lead;
            const float* c = phase(static_cast<int>(k));
            out[o] = start >= 0 && start + n <= inFrames ? dot(in + start, c, n) : dotEdge(in, inFrames, start, c, n);
            k += step_;
            i0 += k / phases_;
            k %= phases_;
        }
        return;
    }
    for (std::int64_t o = 0; o < outFrames; ++o)
    {
        const double t = static_cast<double>(o) * ratio_;
        const auto i0 = static_cast<std::int64_t>(std::floor(t));
        const double f = (t - static_cast<double>(i0)) * phases_;
        const int k = std::min(static_cast<int>(f), phases_ - 1);
        const auto w = static_cast<float>(f - k);
        const std::int64_t start = i0 - lead;
        const float* c0 = phase(k);
        const float* c1 = phase(k + 1);
        float a, b;
        if (start >= 0 && start + n <= inFrames)
            a = dot(in + start, c0, n), b = dot(in + start, c1, n);
        else
            a = dotEdge(in, inFrames, start, c0, n), b = dotEdge(in, inFrames, start, c1, n);
        out[o] = a + (b - a) * w;
    }
}
} // namespace services::library
