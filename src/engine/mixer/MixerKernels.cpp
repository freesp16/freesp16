#include "engine/mixer/MixerKernels.h"

#if APP_MIXER_HAVE_NEON
 #include <arm_neon.h>
#endif

// Bit-exact parity with the float32 research model: no fused multiply-add anywhere in this file (MixerKernels.h).
#if defined(__clang__)
 #pragma clang fp contract(off)
#elif defined(__GNUC__)
 #pragma GCC optimize("fp-contract=off")
#endif

namespace engine::mixer::kernels
{
namespace scalar
{
namespace
{
inline float absf(float x) noexcept { return x < 0.0f ? -x : x; }

template <bool kRamp, bool kMute, bool kSend> StereoMeter stripT(const StripArgs& a, int n) noexcept
{
    StereoMeter m;
    for (int i = 0; i < n; ++i)
    {
        const float* g = kRamp ? a.gains + 4 * i : a.gains;
        float xl = a.l[i] * g[PanL];
        float xr = a.r[i] * g[PanR];
        if constexpr (kMute)
        {
            xl = xl * a.mute[i];
            xr = xr * a.mute[i];
        }
        if constexpr (kSend)
        {
            const float sl = xl * g[Send];
            const float sr = xr * g[Send];
            a.sendL[i] = a.sendL[i] + sl;
            a.sendR[i] = a.sendR[i] + sr;
        }
        xl = xl * g[Fader];
        xr = xr * g[Fader];
        a.l[i] = xl;
        a.r[i] = xr;
        a.mainL[i] = a.mainL[i] + xl;
        a.mainR[i] = a.mainR[i] + xr;
        const float al = absf(xl), ar = absf(xr);
        m.peakL = al > m.peakL ? al : m.peakL;
        m.peakR = ar > m.peakR ? ar : m.peakR;
        m.sumL += xl * xl;
        m.sumR += xr * xr;
    }
    return m;
}
} // namespace

StereoMeter strip(const StripArgs& a, int n) noexcept
{
    const bool send = a.sendL != nullptr;
    const bool mute = a.mute != nullptr;
    if (a.rampGains)
    {
        if (mute)
            return send ? stripT<true, true, true>(a, n) : stripT<true, true, false>(a, n);
        return send ? stripT<true, false, true>(a, n) : stripT<true, false, false>(a, n);
    }
    if (mute)
        return send ? stripT<false, true, true>(a, n) : stripT<false, true, false>(a, n);
    return send ? stripT<false, false, true>(a, n) : stripT<false, false, false>(a, n);
}

StereoMeter unity(const float* l, const float* r, float* mainL, float* mainR, int n) noexcept
{
    StereoMeter m;
    for (int i = 0; i < n; ++i)
    {
        mainL[i] = mainL[i] + l[i];
        mainR[i] = mainR[i] + r[i];
        const float al = absf(l[i]), ar = absf(r[i]);
        m.peakL = al > m.peakL ? al : m.peakL;
        m.peakR = ar > m.peakR ? ar : m.peakR;
        m.sumL += l[i] * l[i];
        m.sumR += r[i] * r[i];
    }
    return m;
}

void rampFill(const Ramp4& s, float* out, float* accOut, int n) noexcept
{
    for (int k = 0; k < kLanes; ++k)
    {
        float acc = s.cur[k];
        for (int i = 0; i < n; ++i)
        {
            out[4 * i + k] = i >= s.end[k] ? s.target[k] : acc;
            if (i >= s.start[k] && i < s.end[k])
                acc = acc + s.step[k];
        }
        accOut[k] = acc;
    }
}

StereoMeter meter(const float* l, const float* r, int n) noexcept
{
    StereoMeter m;
    for (int i = 0; i < n; ++i)
    {
        const float al = absf(l[i]), ar = absf(r[i]);
        m.peakL = al > m.peakL ? al : m.peakL;
        m.peakR = ar > m.peakR ? ar : m.peakR;
        m.sumL += l[i] * l[i];
        m.sumR += r[i] * r[i];
    }
    return m;
}
} // namespace scalar

#if APP_MIXER_HAVE_NEON
namespace neon
{
// ARMv7 subset only (no vmaxvq/vaddvq: those are AArch64).
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

struct MeterAcc
{
    float32x4_t pl = vdupq_n_f32(0.0f), pr = vdupq_n_f32(0.0f), sl = vdupq_n_f32(0.0f), sr = vdupq_n_f32(0.0f);
    void add(float32x4_t xl, float32x4_t xr) noexcept
    {
        pl = vmaxq_f32(pl, vabsq_f32(xl));
        pr = vmaxq_f32(pr, vabsq_f32(xr));
        sl = vaddq_f32(sl, vmulq_f32(xl, xl));
        sr = vaddq_f32(sr, vmulq_f32(xr, xr));
    }
    StereoMeter result() const noexcept { return {hmax(pl), hmax(pr), hsum(sl), hsum(sr)}; }
};

template <bool kRamp, bool kMute, bool kSend> StereoMeter stripT(const StripArgs& a, int n) noexcept
{
    MeterAcc m;
    float32x4_t cpl {}, cpr {}, cs {}, cf {};
    if constexpr (! kRamp)
    {
        cpl = vdupq_n_f32(a.gains[PanL]);
        cpr = vdupq_n_f32(a.gains[PanR]);
        cs = vdupq_n_f32(a.gains[Send]);
        cf = vdupq_n_f32(a.gains[Fader]);
    }
    for (int i = 0; i < n; i += 4)
    {
        float32x4_t pl = cpl, pr = cpr, gs = cs, gf = cf;
        if constexpr (kRamp)
        {
            const float32x4x4_t g = vld4q_f32(a.gains + 4 * i); // de-interleaves 4 frames × 4 lanes
            pl = g.val[PanL];
            pr = g.val[PanR];
            gs = g.val[Send];
            gf = g.val[Fader];
        }
        float32x4_t xl = vmulq_f32(vld1q_f32(a.l + i), pl);
        float32x4_t xr = vmulq_f32(vld1q_f32(a.r + i), pr);
        if constexpr (kMute)
        {
            const float32x4_t mg = vld1q_f32(a.mute + i);
            xl = vmulq_f32(xl, mg);
            xr = vmulq_f32(xr, mg);
        }
        if constexpr (kSend)
        {
            vst1q_f32(a.sendL + i, vaddq_f32(vld1q_f32(a.sendL + i), vmulq_f32(xl, gs)));
            vst1q_f32(a.sendR + i, vaddq_f32(vld1q_f32(a.sendR + i), vmulq_f32(xr, gs)));
        }
        xl = vmulq_f32(xl, gf);
        xr = vmulq_f32(xr, gf);
        vst1q_f32(a.l + i, xl);
        vst1q_f32(a.r + i, xr);
        vst1q_f32(a.mainL + i, vaddq_f32(vld1q_f32(a.mainL + i), xl));
        vst1q_f32(a.mainR + i, vaddq_f32(vld1q_f32(a.mainR + i), xr));
        m.add(xl, xr);
    }
    return m.result();
}
} // namespace

StereoMeter strip(const StripArgs& a, int n) noexcept
{
    const bool send = a.sendL != nullptr;
    const bool mute = a.mute != nullptr;
    if (a.rampGains)
    {
        if (mute)
            return send ? stripT<true, true, true>(a, n) : stripT<true, true, false>(a, n);
        return send ? stripT<true, false, true>(a, n) : stripT<true, false, false>(a, n);
    }
    if (mute)
        return send ? stripT<false, true, true>(a, n) : stripT<false, true, false>(a, n);
    return send ? stripT<false, false, true>(a, n) : stripT<false, false, false>(a, n);
}

StereoMeter unity(const float* l, const float* r, float* mainL, float* mainR, int n) noexcept
{
    MeterAcc m;
    for (int i = 0; i < n; i += 4)
    {
        const float32x4_t xl = vld1q_f32(l + i), xr = vld1q_f32(r + i);
        vst1q_f32(mainL + i, vaddq_f32(vld1q_f32(mainL + i), xl));
        vst1q_f32(mainR + i, vaddq_f32(vld1q_f32(mainR + i), xr));
        m.add(xl, xr);
    }
    return m.result();
}

void rampFill(const Ramp4& s, float* out, float* accOut, int n) noexcept
{
    float32x4_t acc = vld1q_f32(s.cur);
    const float32x4_t step = vld1q_f32(s.step), target = vld1q_f32(s.target);
    // Fast path (the usual block of a ramp): every lane either idle (start = end = 0, step 0, cur = target) or ramping
    // through the whole block (start 0, end > n): no per-frame selects, one add per frame. Same values as below.
    bool whole = true;
    for (int k = 0; k < kLanes; ++k)
        whole = whole && s.start[k] == 0 &&
                ((s.end[k] == 0 && s.step[k] == 0.0f && s.cur[k] == s.target[k]) || s.end[k] > n);
    if (whole)
    {
        for (int i = 0; i < n; ++i)
        {
            vst1q_f32(out + 4 * i, acc);
            acc = vaddq_f32(acc, step);
        }
        vst1q_f32(accOut, acc);
        return;
    }
    const int32x4_t start = vld1q_s32(s.start), end = vld1q_s32(s.end), one = vdupq_n_s32(1);
    int32x4_t idx = vdupq_n_s32(0);
    for (int i = 0; i < n; ++i)
    {
        const uint32x4_t after = vcgeq_s32(idx, end);
        const uint32x4_t inRamp = vandq_u32(vcgeq_s32(idx, start), vcltq_s32(idx, end));
        vst1q_f32(out + 4 * i, vbslq_f32(after, target, acc));
        acc = vbslq_f32(inRamp, vaddq_f32(acc, step), acc);
        idx = vaddq_s32(idx, one);
    }
    vst1q_f32(accOut, acc);
}

StereoMeter meter(const float* l, const float* r, int n) noexcept
{
    MeterAcc m;
    for (int i = 0; i < n; i += 4)
        m.add(vld1q_f32(l + i), vld1q_f32(r + i));
    return m.result();
}
} // namespace neon
#endif
} // namespace engine::mixer::kernels
