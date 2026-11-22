#include "engine/voice/stretch/StretchKernels.h"

#if APP_ENGINE_HAVE_NEON
 #include <arm_neon.h>
#endif

namespace engine::voice::stretch::kernels
{
namespace
{
// atan(a) = a·P(a²) on [0, 1], least-squares fit on Chebyshev nodes (|error| < 5e-8 before float rounding).
constexpr float kA0 = 0.9999994367748251f, kA1 = -0.3333010635261202f, kA2 = 0.1994850483461732f,
                kA3 = -0.13915779661182806f, kA4 = 0.09656194575621337f, kA5 = -0.05606228148502346f,
                kA6 = 0.02194595844354606f, kA7 = -0.0040731207350607045f;
constexpr float kTiny = 1.0e-30f;

inline float atanPoly(float a) noexcept
{
    const float s = a * a;
    float t = kA7;
    t = kA6 + s * t;
    t = kA5 + s * t;
    t = kA4 + s * t;
    t = kA3 + s * t;
    t = kA2 + s * t;
    t = kA1 + s * t;
    t = kA0 + s * t;
    return a * t;
}
} // namespace

namespace scalar
{
void mulWindow(float* dst, const float* src, const float* win, int n) noexcept
{
    for (int i = 0; i < n; ++i)
        dst[i] = src[i] * win[i];
}

void addWindowed(float* acc, const float* src, const float* win, int n) noexcept
{
    for (int i = 0; i < n; ++i)
        acc[i] += src[i] * win[i];
}

float dot(const float* a, const float* b, int n) noexcept
{
    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
    int i = 0;
    for (; i + 4 <= n; i += 4)
    {
        s0 += a[i] * b[i];
        s1 += a[i + 1] * b[i + 1];
        s2 += a[i + 2] * b[i + 2];
        s3 += a[i + 3] * b[i + 3];
    }
    float s = (s0 + s2) + (s1 + s3);
    for (; i < n; ++i)
        s += a[i] * b[i];
    return s;
}

void butterflies(float* re, float* im, const float* wr, const float* wi, int half, int n) noexcept
{
    for (int b = 0; b < n; b += 2 * half)
    {
        float* ar = re + b;
        float* ai = im + b;
        float* br = ar + half;
        float* bi = ai + half;
        for (int j = 0; j < half; ++j)
        {
            const float tr = br[j] * wr[j] - bi[j] * wi[j];
            const float ti = br[j] * wi[j] + bi[j] * wr[j];
            br[j] = ar[j] - tr;
            bi[j] = ai[j] - ti;
            ar[j] = ar[j] + tr;
            ai[j] = ai[j] + ti;
        }
    }
}

void magSq(float* out, const float* re, const float* im, int n) noexcept
{
    for (int i = 0; i < n; ++i)
        out[i] = re[i] * re[i] + im[i] * im[i];
}

void atan2(float* out, const float* y, const float* x, int n) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const float ax = x[i] < 0.0f ? -x[i] : x[i];
        const float ay = y[i] < 0.0f ? -y[i] : y[i];
        const float mx = ax > ay ? ax : ay;
        const float mn = ax > ay ? ay : ax;
        const float a = mn / (mx > kTiny ? mx : kTiny);
        float r = atanPoly(a);
        r = ay > ax ? kPi * 0.5f - r : r;
        r = x[i] < 0.0f ? kPi - r : r;
        out[i] = y[i] < 0.0f ? -r : r;
    }
}

void rotate(float* re, float* im, const float* cr, const float* ci, int n) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const float r = re[i] * cr[i] - im[i] * ci[i];
        const float m = re[i] * ci[i] + im[i] * cr[i];
        re[i] = r;
        im[i] = m;
    }
}
} // namespace scalar

#if APP_ENGINE_HAVE_NEON
namespace neon
{
void mulWindow(float* dst, const float* src, const float* win, int n) noexcept
{
    int i = 0;
    for (; i + 4 <= n; i += 4)
        vst1q_f32(dst + i, vmulq_f32(vld1q_f32(src + i), vld1q_f32(win + i)));
    for (; i < n; ++i)
        dst[i] = src[i] * win[i];
}

void addWindowed(float* acc, const float* src, const float* win, int n) noexcept
{
    int i = 0;
    for (; i + 4 <= n; i += 4)
        vst1q_f32(acc + i, vmlaq_f32(vld1q_f32(acc + i), vld1q_f32(src + i), vld1q_f32(win + i)));
    for (; i < n; ++i)
        acc[i] += src[i] * win[i];
}

float dot(const float* a, const float* b, int n) noexcept
{
    float32x4_t acc = vdupq_n_f32(0.0f);
    int i = 0;
    for (; i + 4 <= n; i += 4)
        acc = vmlaq_f32(acc, vld1q_f32(a + i), vld1q_f32(b + i));
    const float32x2_t h = vadd_f32(vget_low_f32(acc), vget_high_f32(acc)); // (s0 + s2, s1 + s3)
    float s = vget_lane_f32(vpadd_f32(h, h), 0);
    for (; i < n; ++i)
        s += a[i] * b[i];
    return s;
}

void butterflies(float* re, float* im, const float* wr, const float* wi, int half, int n) noexcept
{
    if ((half & 3) != 0)
    {
        scalar::butterflies(re, im, wr, wi, half, n);
        return;
    }
    for (int b = 0; b < n; b += 2 * half)
    {
        float* ar = re + b;
        float* ai = im + b;
        float* br = ar + half;
        float* bi = ai + half;
        for (int j = 0; j < half; j += 4)
        {
            const float32x4_t xr = vld1q_f32(br + j), xi = vld1q_f32(bi + j);
            const float32x4_t cr = vld1q_f32(wr + j), ci = vld1q_f32(wi + j);
            const float32x4_t tr = vmlsq_f32(vmulq_f32(xr, cr), xi, ci); // xr·cr − xi·ci
            const float32x4_t ti = vmlaq_f32(vmulq_f32(xr, ci), xi, cr); // xr·ci + xi·cr
            const float32x4_t yr = vld1q_f32(ar + j), yi = vld1q_f32(ai + j);
            vst1q_f32(br + j, vsubq_f32(yr, tr));
            vst1q_f32(bi + j, vsubq_f32(yi, ti));
            vst1q_f32(ar + j, vaddq_f32(yr, tr));
            vst1q_f32(ai + j, vaddq_f32(yi, ti));
        }
    }
}

void magSq(float* out, const float* re, const float* im, int n) noexcept
{
    int i = 0;
    for (; i + 4 <= n; i += 4)
    {
        const float32x4_t r = vld1q_f32(re + i), m = vld1q_f32(im + i);
        vst1q_f32(out + i, vmlaq_f32(vmulq_f32(r, r), m, m));
    }
    for (; i < n; ++i)
        out[i] = re[i] * re[i] + im[i] * im[i];
}

void atan2(float* out, const float* y, const float* x, int n) noexcept
{
    const float32x4_t halfPi = vdupq_n_f32(kPi * 0.5f), pi = vdupq_n_f32(kPi), tiny = vdupq_n_f32(kTiny);
    const float32x4_t zero = vdupq_n_f32(0.0f);
    int i = 0;
    for (; i + 4 <= n; i += 4)
    {
        const float32x4_t vx = vld1q_f32(x + i), vy = vld1q_f32(y + i);
        const float32x4_t ax = vabsq_f32(vx), ay = vabsq_f32(vy);
        const float32x4_t mx = vmaxq_f32(vmaxq_f32(ax, ay), tiny);
        const float32x4_t mn = vminq_f32(ax, ay);
        float32x4_t e = vrecpeq_f32(mx);
        e = vmulq_f32(e, vrecpsq_f32(mx, e));
        e = vmulq_f32(e, vrecpsq_f32(mx, e));
        const float32x4_t a = vmulq_f32(mn, e);
        const float32x4_t s = vmulq_f32(a, a);
        float32x4_t t = vdupq_n_f32(kA7);
        t = vmlaq_f32(vdupq_n_f32(kA6), s, t);
        t = vmlaq_f32(vdupq_n_f32(kA5), s, t);
        t = vmlaq_f32(vdupq_n_f32(kA4), s, t);
        t = vmlaq_f32(vdupq_n_f32(kA3), s, t);
        t = vmlaq_f32(vdupq_n_f32(kA2), s, t);
        t = vmlaq_f32(vdupq_n_f32(kA1), s, t);
        t = vmlaq_f32(vdupq_n_f32(kA0), s, t);
        float32x4_t r = vmulq_f32(a, t);
        r = vbslq_f32(vcgtq_f32(ay, ax), vsubq_f32(halfPi, r), r);
        r = vbslq_f32(vcltq_f32(vx, zero), vsubq_f32(pi, r), r);
        r = vbslq_f32(vcltq_f32(vy, zero), vnegq_f32(r), r);
        vst1q_f32(out + i, r);
    }
    scalar::atan2(out + i, y + i, x + i, n - i);
}

void rotate(float* re, float* im, const float* cr, const float* ci, int n) noexcept
{
    int i = 0;
    for (; i + 4 <= n; i += 4)
    {
        const float32x4_t r = vld1q_f32(re + i), m = vld1q_f32(im + i);
        const float32x4_t c = vld1q_f32(cr + i), s = vld1q_f32(ci + i);
        vst1q_f32(re + i, vmlsq_f32(vmulq_f32(r, c), m, s));
        vst1q_f32(im + i, vmlaq_f32(vmulq_f32(r, s), m, c));
    }
    scalar::rotate(re + i, im + i, cr + i, ci + i, n - i);
}
} // namespace neon
#endif
} // namespace engine::voice::stretch::kernels
