// Building blocks of the Multi-mode filter and the 2-band EQ (spec 02-features/effects/{multimode-filter,
// parametric-eq,lofi}.md; research docs/effects/{multimode-filter,parametric-eq,lofi}.md, CC BY 4.0). Operation for
// operation equal to the shared-block models of the MIT research models tools/fx/lofi.py (LinearParameter, Smooth,
// SignalGate, IirFilter biquad and its bilinear LP/HP/BP designs) and tools/fx/mmf.py / peq.py (GainRamp), pinned commit
// 8901c97. Header-only, inline, no allocation; every block function takes n <= 64 samples (n = 0 allowed: a biquad
// still re-seeds and clears its flag, as the reference's process call with an empty chunk does).
//
//   RefParam    the reference LinearParameter: float or choice kind, "last" vs "current" change detection
//               (float: |last − cur| > FLT_MIN; choice: last != cur). `untrap()` makes the next changed() true
//               whatever the value — the Sixteen profile's FX-056 (no change-detection trap, FX-581).
//   RefSmooth   the reference SmoothFloat as used by the SignalGate: N-sample accumulated linear ramp, first sample =
//               start value, inc = f32(f32(target − cur) · f32(1/N)).
//   RefGate     the 0/1 SignalGate (built closed) on a RefSmooth: apply() multiplies by the per-sample gains while it
//               ramps, by the settled gain otherwise (skipped at 1, zero below FLT_EPSILON).
//   RefGainRamp the GainRamp output stage: linear ramp over N samples (scalar accumulation), then a plain multiply.
//   Iir         the direct-form-I biquad of the reference (IirFilter 0xe16dc and IirFilter2 0xe27b8): L/R in two lanes,
//               y = f32(f32(f32(f32(b0·x + b1·x1) + b2·x2) − a1·y1) − a2·y2) (each product rounded, never fused), a
//               4-deep input history and the re-seed of FX-427: directly after a coefficient change with the flag set
//               (and Q > 0) the last four inputs are re-filtered with the new coefficients. The k·y_prev term of
//               lofi.py's IirFilter is dropped: k is 0 in every filter here, and x + 0·y_prev = x on finite inputs.
//   bilinear    the shared LP / HP / BP coefficient routines (lofi.md "Filter", 0xe1ca0 / 0xe1bb8 / 0xe1d88), double
//               maths, float32 stores.
//
// Scalar and NEON: iirRun<Neon> / iirRun2<Neon> (one or two cascaded biquads) run L/R in two float32x2 lanes on NEON and
// lane after lane in the scalar reference, with the same operations in the same order, so both are bit-equal on normal
// inputs (tests/unit/fx/test_fx_filter.cpp neon_vs_scalar_*). NEON flushes denormals; so does the app (FTZ, FX-055).
#pragma once

#include "engine/fx/dsp/Numerics.h"
#include "engine/fx/dsp/Vec.h"

#include <cmath>
#include <cstdint>
#include <limits>

#if APP_FX_HAVE_NEON
#include <arm_neon.h>
#endif

namespace engine::fx::dsp::filt
{
constexpr float kFltMin = 1.17549435e-38f;
constexpr int kMaxN = 64;

// round half to even of a float value (the reference's double + 1.5·2^52 trick; Python round())
inline int rnd(float x) noexcept
{
    return static_cast<int>(std::nearbyint(static_cast<double>(x)));
}

// 3 ms in samples: roundToInt(f32(f32(fs) · 0.001) · 3), half to even — 132 @ 44.1 kHz, 144 @ 48 kHz, 288 @ 96 kHz
inline int gateSamples(double fs) noexcept
{
    APP_FX_NOFUSE
    return rnd((static_cast<float>(fs) * 0.001f) * 3.0f);
}

// ------------------------------------------------------------------------------------------------ RefParam

struct RefParam
{
    bool choice = false;
    bool untrapped = false; // next changed() is true (Sixteen: FX-056 / FX-581)
    float lastF = 0.0f, curF = 0.0f;
    int lastI = 0, curI = 0;

    void init(bool isChoice, float ctorValue) noexcept
    {
        choice = isChoice;
        untrapped = false;
        lastF = curF = ctorValue;
        lastI = curI = static_cast<int>(ctorValue);
    }
    void set(float v) noexcept { curF = v; }
    void setChoice(int v) noexcept { curI = v; }
    bool changed() noexcept
    {
        APP_FX_NOFUSE
        if (untrapped)
        {
            untrapped = false;
            lastF = curF;
            lastI = curI;
            return true;
        }
        if (choice)
        {
            const bool c = lastI != curI;
            if (c)
                lastI = curI;
            return c;
        }
        const float d = lastF - curF;
        if (d <= kFltMin && -kFltMin <= d)
            return false;
        lastF = curF;
        return true;
    }
    float get() const noexcept { return choice ? static_cast<float>(curI) : curF; }
};

// ------------------------------------------------------------------------------------------------ RefSmooth / RefGate

struct RefSmooth
{
    int state = 0;
    float cur = 0.0f, target = 0.0f, inc = 0.0f;
    int rem = 0, N = 1;
    float invN = 1.0f;

    void init(int n) noexcept
    {
        N = n;
        invN = 1.0f / static_cast<float>(n);
        setNow(0.0f);
    }
    void setNow(float v) noexcept
    {
        state = 0;
        cur = target = v;
        inc = 0.0f;
        rem = 0;
    }
    void setTarget(float v) noexcept
    {
        APP_FX_NOFUSE
        if (nearlyEqual(v, target))
            return;
        target = v;
        state = 1;
        rem = N;
        inc = (v - cur) * invN;
    }
    // per-sample gains of the next n samples (only called while state != 0)
    void gains(float* g, int n) noexcept
    {
        APP_FX_NOFUSE
        if (n < 1)
            return;
        float c = cur;
        int i = 0;
        if (state != 0)
        {
            if (n < rem)
            {
                for (; i < n; ++i)
                {
                    g[i] = c;
                    c = c + inc;
                }
                cur = c;
                rem -= n;
                return;
            }
            for (; i < rem; ++i)
            {
                g[i] = c;
                c = c + inc;
            }
            state = 0;
            c = target;
            rem = 0;
        }
        for (; i < n; ++i)
            g[i] = c;
        cur = c;
    }
};

template <bool Neon> inline void mulConst(float* x, float g, int n) noexcept
{
#if APP_FX_HAVE_NEON
    if constexpr (Neon)
    {
        vec::neon::mul(x, g, n);
        return;
    }
#endif
    vec::scalar::mul(x, g, n);
}
template <bool Neon> inline void mulGains(float* x, const float* g, int n) noexcept
{
#if APP_FX_HAVE_NEON
    if constexpr (Neon)
    {
        vec::neon::mulv(x, g, n);
        return;
    }
#endif
    vec::scalar::mulv(x, g, n);
}
inline void zeroFill(float* x, int n) noexcept
{
    for (int i = 0; i < n; ++i)
        x[i] = 0.0f;
}

struct RefGate
{
    RefSmooth r;
    static constexpr float kOff = 0.0f, kOn = 1.0f;

    void init(int n) noexcept { r.init(n); } // built closed
    void open() noexcept { r.setTarget(kOn); }
    void close() noexcept { r.setTarget(kOff); }
    void setNow(bool on) noexcept { r.setNow(on ? kOn : kOff); }
    bool fadingOut() const noexcept { return r.state == 1 && nearlyEqual(r.target, kOff); }
    bool closedSettled() const noexcept { return r.state != 1 && nearlyEqual(r.cur, kOff); }

    template <bool Neon> void apply(float* l, float* rr, int n) noexcept
    {
        if (n <= 0)
            return;
        const float v = r.state == 1 ? r.target : r.cur;
        if (! nearlyEqual(v, kOff) && ! nearlyEqual(v, kOn))
        {
            zeroFill(l, n);
            zeroFill(rr, n);
            return;
        }
        if (r.state == 1)
        {
            alignas(16) float g[kMaxN];
            r.gains(g, n);
            mulGains<Neon>(l, g, n);
            mulGains<Neon>(rr, g, n);
            return;
        }
        const float c = r.cur;
        if (nearlyEqual(c, 1.0f))
            return;
        if (absf(c) < kEps)
        {
            zeroFill(l, n);
            zeroFill(rr, n);
            return;
        }
        mulConst<Neon>(l, c, n);
        mulConst<Neon>(rr, c, n);
    }
};

// ------------------------------------------------------------------------------------------------ RefGainRamp

struct RefGainRamp
{
    float inc = 0.0f, target = 1.0f, cur = 1.0f;
    int rem = 0, N = 1;
    float invN = 1.0f;
    bool idle = true;
    int state = 0; // 0 hold, 1 ramp (2 = delayed start: never used by these effects, delay 0)

    void init(int n, float g) noexcept
    {
        inc = 0.0f;
        target = cur = g;
        rem = 0;
        N = n > 0 ? n : 1;
        invN = n > 0 ? 1.0f / static_cast<float>(n) : 1.0f;
        idle = true;
        state = 0;
    }
    void setTarget(float g) noexcept // force 1, delay 0 (both effects)
    {
        APP_FX_NOFUSE
        if (! idle)
            return;
        if (! (target > g || target < g))
            return;
        inc = (g - cur) * invN;
        target = g;
        rem = N;
        state = 1;
        idle = true;
    }
    void setNow(float g) noexcept
    {
        rem = 0;
        state = 0;
        cur = target = g;
        inc = 0.0f;
        idle = true;
    }
    template <bool Neon> void process(float* l, float* r, int n) noexcept
    {
        APP_FX_NOFUSE
        int i = 0;
        while (n > 0)
        {
            if (state == 1)
            {
                float g = cur;
                const int k = n < rem ? n : rem;
                alignas(16) float gs[kMaxN];
                for (int j = 0; j < k; ++j)
                {
                    gs[j] = g;
                    g = g + inc;
                }
                mulGains<Neon>(l + i, gs, k);
                mulGains<Neon>(r + i, gs, k);
                if (n < rem)
                {
                    cur = g;
                    rem -= n;
                    return;
                }
                i += rem;
                n -= rem;
                rem = 0;
                state = 0;
                cur = target;
                inc = 0.0f;
                idle = true;
            }
            else
            {
                if (cur != 1.0f) // x · 1 = x
                {
                    mulConst<Neon>(l + i, cur, n);
                    mulConst<Neon>(r + i, cur, n);
                }
                return;
            }
        }
    }
};

// ------------------------------------------------------------------------------------------------ Iir (DF-I biquad)

struct Coefs
{
    float b0 = 0.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
};

struct Iir
{
    Coefs c{};
    float x[4][2]{}; // x[i][lane] = input n−1−i
    float y[2][2]{}; // y[i][lane] = output n−1−i
    bool reseed = false;

    void clearTaps() noexcept
    {
        for (auto& v : x)
            v[0] = v[1] = 0.0f;
        for (auto& v : y)
            v[0] = v[1] = 0.0f;
    }
    // FX-427 / 0xe1844: re-filter the last four inputs with the current coefficients (both lanes).
    void reseedNow() noexcept
    {
        APP_FX_NOFUSE
        const Coefs k = c;
        for (int ch = 0; ch < 2; ++ch)
        {
            const float x1 = x[0][ch], x2 = x[1][ch], x3 = x[2][ch], x4 = x[3][ch];
            const float y4 = k.b0 * x4;
            const float y3 = (k.b0 * x3 + k.b1 * x4) - k.a1 * y4;
            const float y2 = (((k.b0 * x2 + k.b1 * x3) + k.b2 * x4) - k.a1 * y3) - k.a2 * y4;
            const float y1 = (((k.b0 * x1 + k.b1 * x2) + k.b2 * x3) - k.a1 * y2) - k.a2 * y3;
            y[0][ch] = y1;
            y[1][ch] = y2;
        }
    }
    void begin(bool qPositive) noexcept
    {
        if (reseed && qPositive)
            reseedNow();
        reseed = false;
    }
};

namespace detail
{
inline void laneRun(const Coefs& k, float* x4, float* y2, int ch, float* buf, int n) noexcept
{
    APP_FX_NOFUSE
    float x1 = x4[0 * 2 + ch], x2 = x4[1 * 2 + ch], x3 = x4[2 * 2 + ch], xx4 = x4[3 * 2 + ch];
    float y1 = y2[0 * 2 + ch], yy2 = y2[1 * 2 + ch];
    for (int j = 0; j < n; ++j)
    {
        const float in = buf[j];
        float o = k.b0 * in;
        o = o + k.b1 * x1;
        o = o + k.b2 * x2;
        o = o - k.a1 * y1;
        o = o - k.a2 * yy2;
        buf[j] = o;
        xx4 = x3;
        x3 = x2;
        x2 = x1;
        x1 = in;
        yy2 = y1;
        y1 = o;
    }
    x4[0 * 2 + ch] = x1;
    x4[1 * 2 + ch] = x2;
    x4[2 * 2 + ch] = x3;
    x4[3 * 2 + ch] = xx4;
    y2[0 * 2 + ch] = y1;
    y2[1 * 2 + ch] = yy2;
}
} // namespace detail

// Scalar reference: one biquad over n samples, lane after lane (the research model's order).
inline void iirRunScalar(Iir& f, bool qPositive, float* l, float* r, int n) noexcept
{
    f.begin(qPositive);
    if (n <= 0)
        return;
    detail::laneRun(f.c, &f.x[0][0], &f.y[0][0], 0, l, n);
    detail::laneRun(f.c, &f.x[0][0], &f.y[0][0], 1, r, n);
}

#if APP_FX_HAVE_NEON
namespace detail
{
struct Lanes
{
    float32x2_t x1, x2, x3, x4, y1, y2;
    void load(const Iir& f) noexcept
    {
        x1 = vld1_f32(f.x[0]);
        x2 = vld1_f32(f.x[1]);
        x3 = vld1_f32(f.x[2]);
        x4 = vld1_f32(f.x[3]);
        y1 = vld1_f32(f.y[0]);
        y2 = vld1_f32(f.y[1]);
    }
    void store(Iir& f) const noexcept
    {
        vst1_f32(f.x[0], x1);
        vst1_f32(f.x[1], x2);
        vst1_f32(f.x[2], x3);
        vst1_f32(f.x[3], x4);
        vst1_f32(f.y[0], y1);
        vst1_f32(f.y[1], y2);
    }
    // one sample: the scalar reference's operations, both lanes at once (vmul + vadd/vsub, never vmla/vfma)
    float32x2_t step(const Coefs& k, float32x2_t in) noexcept
    {
        APP_FX_NOFUSE
        float32x2_t o = vmul_n_f32(in, k.b0);
        o = vadd_f32(o, vmul_n_f32(x1, k.b1));
        o = vadd_f32(o, vmul_n_f32(x2, k.b2));
        o = vsub_f32(o, vmul_n_f32(y1, k.a1));
        o = vsub_f32(o, vmul_n_f32(y2, k.a2));
        x4 = x3;
        x3 = x2;
        x2 = x1;
        x1 = in;
        y2 = y1;
        y1 = o;
        return o;
    }
};
} // namespace detail

inline void iirRunNeon(Iir& f, bool qPositive, float* l, float* r, int n) noexcept
{
    f.begin(qPositive);
    if (n <= 0)
        return;
    const Coefs k = f.c;
    detail::Lanes s;
    s.load(f);
    for (int j = 0; j < n; ++j)
    {
        const float32x2_t in = vset_lane_f32(r[j], vdup_n_f32(l[j]), 1);
        const float32x2_t o = s.step(k, in);
        l[j] = vget_lane_f32(o, 0);
        r[j] = vget_lane_f32(o, 1);
    }
    s.store(f);
}

// Two cascaded biquads fused in one loop (each sample through stage 1, then stage 2): the same operations as running
// stage 1 over the chunk and then stage 2, so bit-equal to the scalar reference.
inline void iirRun2Neon(Iir& f1, Iir& f2, bool qPositive, float* l, float* r, int n) noexcept
{
    f1.begin(qPositive);
    f2.begin(qPositive);
    if (n <= 0)
        return;
    const Coefs k1 = f1.c, k2 = f2.c;
    detail::Lanes s1, s2;
    s1.load(f1);
    s2.load(f2);
    for (int j = 0; j < n; ++j)
    {
        const float32x2_t in = vset_lane_f32(r[j], vdup_n_f32(l[j]), 1);
        const float32x2_t o = s2.step(k2, s1.step(k1, in));
        l[j] = vget_lane_f32(o, 0);
        r[j] = vget_lane_f32(o, 1);
    }
    s1.store(f1);
    s2.store(f2);
}
#endif

template <bool Neon> inline void iirRun(Iir& f, bool qPositive, float* l, float* r, int n) noexcept
{
#if APP_FX_HAVE_NEON
    if constexpr (Neon)
    {
        iirRunNeon(f, qPositive, l, r, n);
        return;
    }
#endif
    iirRunScalar(f, qPositive, l, r, n);
}

template <bool Neon> inline void iirRun2(Iir& f1, Iir& f2, bool qPositive, float* l, float* r, int n) noexcept
{
#if APP_FX_HAVE_NEON
    if constexpr (Neon)
    {
        iirRun2Neon(f1, f2, qPositive, l, r, n);
        return;
    }
#endif
    iirRunScalar(f1, qPositive, l, r, n);
    iirRunScalar(f2, qPositive, l, r, n);
}

// ------------------------------------------------------------------------------------------------ bilinear designs

enum class Kind : std::uint8_t
{
    LowPass,
    HighPass,
    BandPass
};

// lofi.py Biquad.coefs (0xe1ca0 LPF / 0xe1bb8 HPF / 0xe1d88 BPF): fc in Hz (float32), Q (double), fs the integer rate.
inline Coefs bilinear(Kind kind, float fc32, double Q, double fs) noexcept
{
    APP_FX_NOFUSE
    constexpr double kPi = 3.141592653589793;
    constexpr double kTwoPi = 6.283185307179586;
    constexpr double kInvTwoPi = 0.15915494309189535;
    constexpr double kFourPi2 = 39.47841760435743;
    constexpr double kEightPi2 = 78.95683520871486;
    const double fc = static_cast<double>(fc32);
    Coefs c;
    if (kind == Kind::BandPass)
    {
        if (! (Q > 0.01))
            Q = 0.01;
        const double w = std::tan(fc * (kPi / fs));
        const double iq = 1.0 / Q;
        const double s = std::sqrt(4.0 + iq * iq);
        const double f = w * kInvTwoPi;
        const double lo = (f * 0.5) * (s - iq);
        const double hi = lo + f * iq;
        const double B = (hi - lo) * kTwoPi;
        const double W = (hi * kFourPi2) * lo;
        const double nrm = 1.0 / ((B + 1.0) + W);
        const double a1 = ((W - 1.0) + (W - 1.0)) * nrm;
        const double a2 = ((1.0 - B) + W) * nrm;
        c.b0 = static_cast<float>(B * nrm);
        c.b1 = 0.0f;
        c.a1 = static_cast<float>(a1);
        c.a2 = static_cast<float>(a2);
        c.b2 = -c.b0;
        return c;
    }
    const double w = std::tan(fc * (kPi / fs));
    const double f = w * kInvTwoPi;
    const double wq = ((1.0 / Q) * kTwoPi) * f;
    const double w2 = (f * kFourPi2) * f;
    const double nrm = 1.0 / ((wq + 1.0) + w2);
    const double a1 = ((f * kEightPi2) * f - 2.0) * nrm;
    const double a2 = (w2 + (1.0 - wq)) * nrm;
    if (kind == Kind::HighPass)
    {
        c.b0 = static_cast<float>(nrm);
        c.b2 = c.b0;
        c.b1 = static_cast<float>(nrm * -2.0);
    }
    else
    {
        c.b0 = static_cast<float>(w2 * nrm);
        c.b2 = c.b0;
        c.b1 = c.b0 + c.b0;
    }
    c.a1 = static_cast<float>(a1);
    c.a2 = static_cast<float>(a2);
    return c;
}
} // namespace engine::fx::dsp::filt
