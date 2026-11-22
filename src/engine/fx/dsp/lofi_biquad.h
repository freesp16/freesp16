// LoFi's filter kernel: the reference's IirFilter (spec lofi.md FX-923, research docs/effects/lofi.md
// "Filter", CC BY 4.0; operation for operation as tools/fx/lofi.py class Biquad, MIT) — a direct-form-I biquad with L
// and R in two NEON lanes, coefficients in double rounded to float32, and the 4-deep tap history the reference keeps
// for re-priming after a type switch (FX-924: "the new filter re-filters the last four inputs").
//
//   y = f32(f32(f32(f32(f32(b0·v) + f32(b1·v1)) + f32(b2·v2)) − f32(a1·y1)) − f32(a2·y2))   (never fused)
//   HP: b = (1, −2, 1)·nrm   LP: b = (w², 2w², w²)·nrm with w = tan(π·fc/fs) / 2π·2π (the reference's order)
//   BP: the f_lo / f_hi construction, Q floored at 0.01
//
// processScalar() is the reference; processNeon() runs both channels in one float32x2_t with the same operation order
// (vmul + vadd/vsub, never vmla/vfma), so it is bit-equal on normal inputs (tests/unit/fx_color/test_lofi.cpp).
// Header-only, real-time safe: no allocation, no libm in process (tan only in setCoefs()).
#pragma once

#include "engine/fx/dsp/Numerics.h"

#include <cmath>

#if APP_FX_HAVE_NEON
#include <arm_neon.h>
#endif

namespace engine::fx::dsp::lofi
{
enum class FilterKind : std::uint8_t
{
    HighPass = 0,
    LowPass = 1,
    BandPass = 2,
    Off = 3
};

class Biquad
{
public:
    void init(FilterKind kind, int fs) noexcept // IirFilter ctor 0xe1edc: coefficients 0 until the first configure
    {
        APP_FX_NOFUSE
        kind_ = kind;
        fs_ = fs >= 10 ? fs : 10;
        nyq_ = static_cast<float>(fs / 2);
        fc_ = (nyq_ + 5.0f) * 0.5f;
        q_ = 0.7071067811865476;
        b0_ = b1_ = b2_ = a1_ = a2_ = 0.0f;
        prime_ = false;
        clearTaps();
    }
    void clearTaps() noexcept // the 4-deep {v, y} history of both lanes; coefficients, fc, Q and the prime flag kept
    {
        for (int i = 0; i < 4; ++i)
            v_[i][0] = v_[i][1] = y_[i][0] = y_[i][1] = 0.0f;
    }
    FilterKind kind() const noexcept { return kind_; }
    float nyquist() const noexcept { return nyq_; }
    void setPrime(bool on) noexcept { prime_ = on; }

    // slot 4 (0xe1bb8 / 0xe1ca0 / 0xe1d88): fc already clamped by the container, Q as stored (double).
    void setCoefs(float fc, double q) noexcept
    {
        APP_FX_NOFUSE
        fc_ = fc;
        q_ = q;
        if (kind_ == FilterKind::Off)
            return;
        constexpr double kPi = 3.141592653589793;
        constexpr double kTwoPi = 6.283185307179586;
        constexpr double kInvTwoPi = 0.15915494309189535;
        constexpr double kFourPi2 = 39.47841760435743;
        constexpr double kEightPi2 = 78.95683520871486;
        const double f0 = static_cast<double>(fc), fs = static_cast<double>(fs_);
        if (kind_ == FilterKind::BandPass)
        {
            double Q = q;
            if (! (Q > 0.01))
                Q = 0.01;
            const double w = std::tan(f0 * (kPi / fs));
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
            b0_ = static_cast<float>(B * nrm);
            b1_ = 0.0f;
            a1_ = static_cast<float>(a1);
            a2_ = static_cast<float>(a2);
            b2_ = -b0_;
            return;
        }
        const double w = std::tan(f0 * (kPi / fs));
        const double f = w * kInvTwoPi;
        const double wq = ((1.0 / q) * kTwoPi) * f;
        const double w2 = (f * kFourPi2) * f;
        const double nrm = 1.0 / ((wq + 1.0) + w2);
        const double a1 = ((f * kEightPi2) * f - 2.0) * nrm;
        const double a2 = (w2 + (1.0 - wq)) * nrm;
        if (kind_ == FilterKind::HighPass)
        {
            b0_ = static_cast<float>(nrm);
            b2_ = b0_;
            b1_ = static_cast<float>(nrm * -2.0);
        }
        else
        {
            b0_ = static_cast<float>(w2 * nrm);
            b2_ = b0_;
            b1_ = b0_ + b0_;
        }
        a1_ = static_cast<float>(a1);
        a2_ = static_cast<float>(a2);
    }

    // slot 2 (0xe16dc), in place on n >= 0 samples of each channel; a pending prime re-filters the last four inputs
    // with the current coefficients first (0xe1844), even for n = 0.
    void processScalar(float* l, float* r, int n) noexcept
    {
        APP_FX_NOFUSE
        if (kind_ == FilterKind::Off)
            return;
        const float b0 = b0_, b1 = b1_, b2 = b2_, a1 = a1_, a2 = a2_;
        float* ch[2] = {l, r};
        for (int c = 0; c < 2; ++c)
        {
            float v1 = v_[0][c], v2 = v_[1][c], v3 = v_[2][c], v4 = v_[3][c];
            float y1 = y_[0][c], y2 = y_[1][c], y3 = y_[2][c], y4 = y_[3][c];
            if (prime_ && q_ > 0.0)
                primeLane(v1, v2, v3, v4, y1, y2, y3, y4);
            float* x = ch[c];
            for (int j = 0; j < n; ++j)
            {
                const float v = x[j];
                float o = b0 * v;
                o = o + b1 * v1;
                o = o + b2 * v2;
                o = o - a1 * y1;
                o = o - a2 * y2;
                x[j] = o;
                v4 = v3;
                v3 = v2;
                v2 = v1;
                v1 = v;
                y4 = y3;
                y3 = y2;
                y2 = y1;
                y1 = o;
            }
            v_[0][c] = v1;
            v_[1][c] = v2;
            v_[2][c] = v3;
            v_[3][c] = v4;
            y_[0][c] = y1;
            y_[1][c] = y2;
            y_[2][c] = y3;
            y_[3][c] = y4;
        }
        prime_ = false;
    }

#if APP_FX_HAVE_NEON
    void processNeon(float* l, float* r, int n) noexcept
    {
        APP_FX_NOFUSE
        if (kind_ == FilterKind::Off)
            return;
        float h[8][2];
        for (int c = 0; c < 2; ++c)
        {
            float v1 = v_[0][c], v2 = v_[1][c], v3 = v_[2][c], v4 = v_[3][c];
            float y1 = y_[0][c], y2 = y_[1][c], y3 = y_[2][c], y4 = y_[3][c];
            if (prime_ && q_ > 0.0)
                primeLane(v1, v2, v3, v4, y1, y2, y3, y4);
            h[0][c] = v1, h[1][c] = v2, h[2][c] = v3, h[3][c] = v4;
            h[4][c] = y1, h[5][c] = y2, h[6][c] = y3, h[7][c] = y4;
        }
        float32x2_t v1 = vld1_f32(h[0]), v2 = vld1_f32(h[1]), v3 = vld1_f32(h[2]), v4 = vld1_f32(h[3]);
        float32x2_t y1 = vld1_f32(h[4]), y2 = vld1_f32(h[5]), y3 = vld1_f32(h[6]), y4 = vld1_f32(h[7]);
        const float b0 = b0_, b1 = b1_, b2 = b2_, a1 = a1_, a2 = a2_;
        for (int j = 0; j < n; ++j)
        {
            const float32x2_t v = vset_lane_f32(r[j], vdup_n_f32(l[j]), 1);
            float32x2_t o = vmul_n_f32(v, b0);
            o = vadd_f32(o, vmul_n_f32(v1, b1));
            o = vadd_f32(o, vmul_n_f32(v2, b2));
            o = vsub_f32(o, vmul_n_f32(y1, a1));
            o = vsub_f32(o, vmul_n_f32(y2, a2));
            l[j] = vget_lane_f32(o, 0);
            r[j] = vget_lane_f32(o, 1);
            v4 = v3;
            v3 = v2;
            v2 = v1;
            v1 = v;
            y4 = y3;
            y3 = y2;
            y2 = y1;
            y1 = o;
        }
        vst1_f32(h[0], v1), vst1_f32(h[1], v2), vst1_f32(h[2], v3), vst1_f32(h[3], v4);
        vst1_f32(h[4], y1), vst1_f32(h[5], y2), vst1_f32(h[6], y3), vst1_f32(h[7], y4);
        for (int c = 0; c < 2; ++c)
        {
            for (int i = 0; i < 4; ++i)
            {
                v_[i][c] = h[i][c];
                y_[i][c] = h[4 + i][c];
            }
        }
        prime_ = false;
    }
#endif

    void process(float* l, float* r, int n, bool scalar) noexcept
    {
#if APP_FX_HAVE_NEON
        if (! scalar)
        {
            processNeon(l, r, n);
            return;
        }
#endif
        (void) scalar;
        processScalar(l, r, n);
    }

private:
    void primeLane(float v1, float v2, float v3, float v4, float& y1, float& y2, float& y3, float& y4) const noexcept
    {
        APP_FX_NOFUSE
        const float b0 = b0_, b1 = b1_, b2 = b2_, a1 = a1_, a2 = a2_;
        y4 = b0 * v4;
        y3 = (b0 * v3 + b1 * v4) - a1 * y4;
        y2 = (((b0 * v2 + b1 * v3) + b2 * v4) - a1 * y3) - a2 * y4;
        y1 = (((b0 * v1 + b1 * v2) + b2 * v3) - a1 * y2) - a2 * y3;
    }

    FilterKind kind_ = FilterKind::Off;
    int fs_ = 44100;
    float nyq_ = 22050.0f, fc_ = 11027.5f;
    double q_ = 0.7071067811865476;
    float b0_ = 0, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
    float v_[4][2]{}, y_[4][2]{};
    bool prime_ = false;
};
} // namespace engine::fx::dsp::lofi
