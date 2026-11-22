#include "engine/fx/dsp/Filters.h"

#include <cmath>

#if APP_FX_HAVE_NEON
#include <arm_neon.h>
#endif

namespace engine::fx::dsp
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kInv2Pi = 0.15915494309189535;
constexpr double k2Pi = 2.0 * kPi;

double clampFc(double fc, double fs) noexcept
{
    const double hi = 0.49 * fs;
    return fc < 5.0 ? 5.0 : (fc > hi ? hi : fc);
}
} // namespace

// ---------------------------------------------------------------------------------------------------- Hpf1

void Hpf1::prepare(double fs) noexcept
{
    fs_ = fs;
    fmax_ = static_cast<float>(static_cast<int>(fs) >> 1);
    zero();
    setCutoff(fc_);
}

void Hpf1::setCutoff(float fc) noexcept
{
    APP_FX_NOFUSE
    const float s = fc <= fmax_ ? fc : fmax_;
    fc_ = 5.0f <= s ? s : 5.0f;
    double w = std::tan(static_cast<double>(fc_) * (kPi / fs_));
    w = (w * kInv2Pi) * k2Pi;
    const double nrm = 1.0 / (w + 1.0);
    const double a1 = (w - 1.0) * nrm;
    b0_ = static_cast<float>(nrm);
    b1_ = static_cast<float>(-nrm);
    a1_ = static_cast<float>(a1);
}

void Hpf1::zero() noexcept
{
    v1_[0] = v1_[1] = y1_[0] = y1_[1] = 0.0f;
}

void Hpf1::processScalar(float* l, float* r, int n) noexcept
{
    APP_FX_NOFUSE
    const float b0 = b0_, b1 = b1_, a1 = a1_;
    float* ch[2] = {l, r};
    for (int c = 0; c < 2; ++c)
    {
        float v1 = v1_[c], y1 = y1_[c];
        float* x = ch[c];
        for (int j = 0; j < n; ++j)
        {
            const float v = x[j];
            const float p = b0 * v;
            const float q = b1 * v1;
            const float s = a1 * y1;
            const float o = (p + q) - s;
            x[j] = o;
            v1 = v;
            y1 = o;
        }
        v1_[c] = v1;
        y1_[c] = y1;
    }
}

void Hpf1::process(float* l, float* r, int n) noexcept
{
#if APP_FX_HAVE_NEON
    APP_FX_NOFUSE
    float32x2_t v1 = vld1_f32(v1_), y1 = vld1_f32(y1_);
    for (int j = 0; j < n; ++j)
    {
        const float32x2_t v = vset_lane_f32(r[j], vdup_n_f32(l[j]), 1);
        const float32x2_t pq = vadd_f32(vmul_n_f32(v, b0_), vmul_n_f32(v1, b1_));
        const float32x2_t o = vsub_f32(pq, vmul_n_f32(y1, a1_));
        l[j] = vget_lane_f32(o, 0);
        r[j] = vget_lane_f32(o, 1);
        v1 = v;
        y1 = o;
    }
    vst1_f32(v1_, v1);
    vst1_f32(y1_, y1);
#else
    processScalar(l, r, n);
#endif
}

// ---------------------------------------------------------------------------------------------------- Biquad

BiquadCoefs BiquadCoefs::design(Type t, double fs, double fc, double q, double gainDb) noexcept
{
    APP_FX_NOFUSE
    fc = clampFc(fc, fs);
    q = q < 0.01 ? 0.01 : q;
    const double w0 = k2Pi * fc / fs;
    const double cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / (2.0 * q);
    const double A = std::pow(10.0, gainDb / 40.0);
    double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
    switch (t)
    {
    case Type::LowPass:
        b0 = (1.0 - cw) * 0.5;
        b1 = 1.0 - cw;
        b2 = b0;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cw;
        a2 = 1.0 - alpha;
        break;
    case Type::HighPass:
        b0 = (1.0 + cw) * 0.5;
        b1 = -(1.0 + cw);
        b2 = b0;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cw;
        a2 = 1.0 - alpha;
        break;
    case Type::BandPass: // constant 0 dB peak gain
        b0 = alpha;
        b1 = 0.0;
        b2 = -alpha;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cw;
        a2 = 1.0 - alpha;
        break;
    case Type::Notch:
        b0 = 1.0;
        b1 = -2.0 * cw;
        b2 = 1.0;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cw;
        a2 = 1.0 - alpha;
        break;
    case Type::AllPass:
        b0 = 1.0 - alpha;
        b1 = -2.0 * cw;
        b2 = 1.0 + alpha;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cw;
        a2 = 1.0 - alpha;
        break;
    case Type::Peaking:
        b0 = 1.0 + alpha * A;
        b1 = -2.0 * cw;
        b2 = 1.0 - alpha * A;
        a0 = 1.0 + alpha / A;
        a1 = -2.0 * cw;
        a2 = 1.0 - alpha / A;
        break;
    case Type::LowShelf:
    case Type::HighShelf:
    {
        const double sq = 2.0 * std::sqrt(A) * alpha;
        const double s = t == Type::LowShelf ? 1.0 : -1.0;
        b0 = A * ((A + 1.0) - s * (A - 1.0) * cw + sq);
        b1 = s * 2.0 * A * ((A - 1.0) - s * (A + 1.0) * cw);
        b2 = A * ((A + 1.0) - s * (A - 1.0) * cw - sq);
        a0 = (A + 1.0) + s * (A - 1.0) * cw + sq;
        a1 = -s * 2.0 * ((A - 1.0) + s * (A + 1.0) * cw);
        a2 = (A + 1.0) + s * (A - 1.0) * cw - sq;
        break;
    }
    }
    BiquadCoefs c;
    c.b0 = static_cast<float>(b0 / a0);
    c.b1 = static_cast<float>(b1 / a0);
    c.b2 = static_cast<float>(b2 / a0);
    c.a1 = static_cast<float>(a1 / a0);
    c.a2 = static_cast<float>(a2 / a0);
    return c;
}

void Biquad2::zero() noexcept
{
    for (int c = 0; c < 2; ++c)
        x1_[c] = x2_[c] = y1_[c] = y2_[c] = 0.0f;
}

void Biquad2::processScalar(float* l, float* r, int n) noexcept
{
    APP_FX_NOFUSE
    const BiquadCoefs k = c_;
    float* ch[2] = {l, r};
    for (int c = 0; c < 2; ++c)
    {
        float x1 = x1_[c], x2 = x2_[c], y1 = y1_[c], y2 = y2_[c];
        float* x = ch[c];
        for (int j = 0; j < n; ++j)
        {
            const float in = x[j];
            const float t0 = k.b0 * in;
            const float t1 = k.b1 * x1;
            const float t2 = k.b2 * x2;
            const float t3 = k.a1 * y1;
            const float t4 = k.a2 * y2;
            const float y = (((t0 + t1) + t2) - t3) - t4;
            x[j] = y;
            x2 = x1;
            x1 = in;
            y2 = y1;
            y1 = y;
        }
        x1_[c] = x1;
        x2_[c] = x2;
        y1_[c] = y1;
        y2_[c] = y2;
    }
}

void Biquad2::process(float* l, float* r, int n) noexcept
{
#if APP_FX_HAVE_NEON
    APP_FX_NOFUSE
    const BiquadCoefs k = c_;
    float32x2_t x1 = vld1_f32(x1_), x2 = vld1_f32(x2_), y1 = vld1_f32(y1_), y2 = vld1_f32(y2_);
    for (int j = 0; j < n; ++j)
    {
        const float32x2_t in = vset_lane_f32(r[j], vdup_n_f32(l[j]), 1);
        float32x2_t y = vadd_f32(vmul_n_f32(in, k.b0), vmul_n_f32(x1, k.b1));
        y = vadd_f32(y, vmul_n_f32(x2, k.b2));
        y = vsub_f32(y, vmul_n_f32(y1, k.a1));
        y = vsub_f32(y, vmul_n_f32(y2, k.a2));
        l[j] = vget_lane_f32(y, 0);
        r[j] = vget_lane_f32(y, 1);
        x2 = x1;
        x1 = in;
        y2 = y1;
        y1 = y;
    }
    vst1_f32(x1_, x1);
    vst1_f32(x2_, x2);
    vst1_f32(y1_, y1);
    vst1_f32(y2_, y2);
#else
    processScalar(l, r, n);
#endif
}

// ---------------------------------------------------------------------------------------------------- OnePole

void OnePole::setCutoff(double fs, double fc) noexcept
{
    const double hi = 0.49 * fs;
    fc = fc < 1.0 ? 1.0 : (fc > hi ? hi : fc);
    const double g = std::tan(kPi * fc / fs);
    g_ = static_cast<float>(g / (1.0 + g));
}

void OnePole::lowPass(float* l, float* r, int n) noexcept
{
    APP_FX_NOFUSE
    float* ch[2] = {l, r};
    for (int c = 0; c < 2; ++c)
    {
        float s = s_[c];
        float* x = ch[c];
        for (int j = 0; j < n; ++j)
        {
            const float v = (x[j] - s) * g_;
            const float y = v + s;
            s = y + v;
            x[j] = y;
        }
        s_[c] = s;
    }
}

void OnePole::highPass(float* l, float* r, int n) noexcept
{
    APP_FX_NOFUSE
    float* ch[2] = {l, r};
    for (int c = 0; c < 2; ++c)
    {
        float s = s_[c];
        float* x = ch[c];
        for (int j = 0; j < n; ++j)
        {
            const float v = (x[j] - s) * g_;
            const float y = v + s;
            s = y + v;
            x[j] = x[j] - y;
        }
        s_[c] = s;
    }
}

// ---------------------------------------------------------------------------------------------------- Svf

void Svf::setParams(double fs, double fc, double q) noexcept
{
    fc = clampFc(fc, fs);
    q = q < 0.01 ? 0.01 : q;
    const double g = std::tan(kPi * fc / fs);
    const double k = 1.0 / q;
    const double a1 = 1.0 / (1.0 + g * (g + k));
    k_ = static_cast<float>(k);
    a1_ = static_cast<float>(a1);
    a2_ = static_cast<float>(g * a1);
    a3_ = static_cast<float>(g * g * a1);
}

void Svf::zero() noexcept
{
    ic1_[0] = ic1_[1] = ic2_[0] = ic2_[1] = 0.0f;
}

namespace
{
// One channel, the mode a template parameter and the coefficients in registers (no per-sample switch, no reload
// through `this` after the stores, which may alias).
template <Svf::Mode M>
void svfChannel(float* x, int n, float a1, float a2, float a3, float k, float& ic1s, float& ic2s) noexcept
{
    APP_FX_NOFUSE
    float ic1 = ic1s, ic2 = ic2s;
    for (int j = 0; j < n; ++j)
    {
        const float in = x[j];
        const float v3 = in - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        if constexpr (M == Svf::Mode::LowPass)
            x[j] = v2;
        else if constexpr (M == Svf::Mode::BandPass)
            x[j] = v1;
        else if constexpr (M == Svf::Mode::HighPass)
            x[j] = in - k * v1 - v2;
        else
            x[j] = in - k * v1;
    }
    ic1s = ic1;
    ic2s = ic2;
}

template <Svf::Mode M>
void svfStereo(float* l, float* r, int n, float a1, float a2, float a3, float k, float* ic1, float* ic2) noexcept
{
    svfChannel<M>(l, n, a1, a2, a3, k, ic1[0], ic2[0]);
    svfChannel<M>(r, n, a1, a2, a3, k, ic1[1], ic2[1]);
}
} // namespace

void Svf::process(Mode m, float* l, float* r, int n) noexcept
{
    switch (m)
    {
    case Mode::LowPass:
        svfStereo<Mode::LowPass>(l, r, n, a1_, a2_, a3_, k_, ic1_, ic2_);
        break;
    case Mode::BandPass:
        svfStereo<Mode::BandPass>(l, r, n, a1_, a2_, a3_, k_, ic1_, ic2_);
        break;
    case Mode::HighPass:
        svfStereo<Mode::HighPass>(l, r, n, a1_, a2_, a3_, k_, ic1_, ic2_);
        break;
    case Mode::Notch:
        svfStereo<Mode::Notch>(l, r, n, a1_, a2_, a3_, k_, ic1_, ic2_);
        break;
    }
}
} // namespace engine::fx::dsp
