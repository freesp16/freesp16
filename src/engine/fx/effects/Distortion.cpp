// Distortion — see Distortion.h. Float32 steps are single C++ float operations (never fused), libm (exp, pow) only in
// the setters (parameter changes), never per sample. Addresses are the reference's (research docs/effects/
// distortion.md "1.60 code map"); the bit patterns are the kernel's literals (tools/fx/distortion.py).
#include "engine/fx/effects/Distortion.h"

#include "engine/fx/EffectRegistryOrder.h"
#include "engine/fx/FxShell.h"
#include "engine/fx/dsp/Numerics.h"

#include <bit>
#include <cmath>

#if APP_FX_HAVE_NEON
#include <arm_neon.h>
#endif

namespace engine::fx
{
namespace
{
const float kTenth = 0.1f;                               // 0x3dcccccd
const float kLo0 = std::bit_cast<float>(0x3d90fcbfu);     // 0.1 · 0.70794576 (init)
const float kM3dB = std::bit_cast<float>(0x3f353befu);    // 0.70794576 = 10^(−3/20)
const float k3ms = std::bit_cast<float>(0x3b449ba6u);     // 0.003
const float kW20k = std::bit_cast<float>(0xc7f56fdbu);    // −2π·20000 (init only)
const float kW50 = std::bit_cast<float>(0xc39d1463u);     // −2π·50 (init only)
const float kM2Pi = std::bit_cast<float>(0xc0c90fdbu);    // −6.2831855 (setters)
constexpr float kFMin = 50.0f, kFMax = 20000.0f, kGMax = 100.0f;
const float k07 = std::bit_cast<float>(0x3f333333u);      // 0.7
const float k005 = std::bit_cast<float>(0x3d4ccccdu);     // 0.05

float expf32(float x) noexcept // glibc expf in the reference; exp in double rounded once here (crmath.expf)
{
    return static_cast<float>(std::exp(static_cast<double>(x)));
}

float freqOf(float v) noexcept // slot 14 indices 1..3: f32(20 · pow(10, f32(3v))) in double
{
    APP_FX_NOFUSE
    const float e = v * 3.0f;
    return static_cast<float>(20.0 * std::pow(10.0, static_cast<double>(e)));
}
} // namespace

float Distortion::coefOf(float fc) const noexcept // a = expf(f32(f32(fc · −2π) · (1/fs)))
{
    APP_FX_NOFUSE
    return expf32((fc * kM2Pi) * invFs_);
}

void Distortion::initKernel(int fs) noexcept // 0xfa36c
{
    APP_FX_NOFUSE
    kfs_ = fs;
    const auto fsf = static_cast<float>(fs);
    hi_ = kTenth;
    lo_ = kLo0;
    drive_ = 1.0f;
    g_ = 1.0f;
    inc_ = expf32(1.0f / (fsf * kTenth));
    dec_ = expf32(-1.0f / (fsf * k3ms));
    blend_ = 0.0f;
    reload_ = fs / 50;
    hold_ = reload_;
    invFs_ = 1.0f / fsf;
    const bool ctor = (quirks_ & kDistQuirkCtorCoefs) != 0; // FX-1082
    out_.fc = kFMax;
    out_.a = ctor ? expf32(invFs_ * kW20k) : coefOf(kFMax);
    out_.b = 1.0f - out_.a;
    hp_.fc = kFMin;
    hp_.a = ctor ? expf32(invFs_ * kW50) : coefOf(kFMin);
    hp_.b = 1.0f - hp_.a;
    pre_.fc = kFMax;
    pre_.a = ctor ? expf32(invFs_ * kW20k) : coefOf(kFMax);
    pre_.b = 1.0f - pre_.a;
    for (OnePole* f : {&out_, &hp_, &pre_})
        f->z[0] = f->z[1] = 0.0f;
    pDrive_ = 0.0f;
    out_.p = kFMax;
    hp_.p = kFMin;
    pre_.p = kFMax;
    pThr_ = kTenth;
}

void Distortion::prepare(double sampleRate, int)
{
    fs_ = sampleRate;
    for (float& s : stored_)
        s = 0.0f;
    initKernel((quirks_ & kDistQuirkFixed44k) != 0 ? 44100 : static_cast<int>(sampleRate));
}

void Distortion::setDriveDb(float p) noexcept // 0xfa614
{
    APP_FX_NOFUSE
    if (pDrive_ == p)
        return;
    const float e = p * k005;
    drive_ = static_cast<float>(std::pow(10.0, static_cast<double>(e))); // the reference: fdlibm powf
    blend_ = 0.0f;
    if (p > 0.0f)
    {
        blend_ = e;
        if (e > 1.0f)
            blend_ = 1.0f;
    }
    pDrive_ = p;
}

void Distortion::setCutoff(OnePole& f, float v) noexcept // kernel slots 5/6/7
{
    APP_FX_NOFUSE
    if (f.p == v)
        return;
    float stored = v, fc;
    if (v < kFMin)
        fc = stored = kFMin;
    else if (v > kFMax)
        fc = stored = kFMax;
    else if (v <= kFMax)
        fc = v;
    else
        fc = kFMax; // NaN: the raw value is stored, the coefficient computed from 20 kHz
    f.fc = stored;
    const float a = coefOf(fc);
    f.p = v;
    f.a = a;
    f.b = 1.0f - a;
}

void Distortion::setThreshold(float v) noexcept // kernel slot 8 0xfa07c
{
    APP_FX_NOFUSE
    if (pThr_ == v)
        return;
    float hi, lo;
    if (v < 0.0f)
        hi = lo = 0.0f;
    else if (v > 1.0f)
    {
        hi = 1.0f;
        lo = kM3dB;
    }
    else
    {
        hi = v;
        lo = v * kM3dB;
    }
    hi_ = hi;
    lo_ = lo;
    pThr_ = hi;
}

void Distortion::apply(int k, float v) noexcept // slot 14 0x1164d8
{
    APP_FX_NOFUSE
    switch (k)
    {
    case 0:
        setDriveDb(v * 40.0f);
        break;
    case 1:
        setCutoff(out_, freqOf(v));
        break;
    case 2:
        setCutoff(hp_, freqOf(v));
        break;
    case 3:
        setCutoff(pre_, freqOf(v));
        break;
    case 4:
        setThreshold(v);
        break;
    default: // 5 = mix: the shell's crossfade (FX-1024)
        break;
    }
}

void Distortion::setParam(int index, float value01) noexcept
{
    if (index < 0 || index >= 6)
        return;
    apply(index, value01);
    stored_[index] = value01;
}

void Distortion::setOption(int which, int value) noexcept
{
    if (which == kClearState && value != 0)
        prepare(fs_, kBlockFrames);
}

void Distortion::reset() noexcept // slot 27 0x115b0c: kernel slot 2 re-init, then slot 14 for i = 0…5
{
    initKernel((quirks_ & kDistQuirkFixed44k) != 0 ? 44100 : static_cast<int>(fs_));
    for (int k = 0; k < 6; ++k)
        apply(k, stored_[k]);
}

float Distortion::shape(float y, float drive, float blend) noexcept
{
    APP_FX_NOFUSE
    float x = y * drive;
    x = x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x);
    const float c = x * (x * x);
    const float s = c * -0.5f + x * 1.5f;
    float t = s < 0.0f ? s + 1.0f : 1.0f - s;
    t = t * (t * t);
    const float w = blend * t;
    return (w * c + (1.0f - w) * s) * k07;
}

void Distortion::highPass(float* l, float* r) noexcept
{
    APP_FX_NOFUSE
    constexpr int n = kBlockFrames;
    const float a = hp_.a, b = hp_.b;
#if APP_FX_HAVE_NEON
    if (! scalar_)
    {
        float32x2_t z = vld1_f32(hp_.z);
        for (int i = 0; i < n; ++i)
        {
            const float32x2_t x = vset_lane_f32(r[i], vdup_n_f32(l[i]), 1);
            z = vadd_f32(vmul_n_f32(x, b), vmul_n_f32(z, a));
            const float32x2_t y = vsub_f32(x, z);
            l[i] = vget_lane_f32(y, 0);
            r[i] = vget_lane_f32(y, 1);
        }
        vst1_f32(hp_.z, z);
        return;
    }
#endif
    float zl = hp_.z[0], zr = hp_.z[1];
    for (int i = 0; i < n; ++i)
    {
        zl = b * l[i] + a * zl;
        l[i] = l[i] - zl;
        zr = b * r[i] + a * zr;
        r[i] = r[i] - zr;
    }
    hp_.z[0] = zl;
    hp_.z[1] = zr;
}

void Distortion::lowPass(OnePole& f, float* l, float* r) noexcept
{
    APP_FX_NOFUSE
    constexpr int n = kBlockFrames;
    const float a = f.a, b = f.b;
#if APP_FX_HAVE_NEON
    if (! scalar_)
    {
        float32x2_t y = vld1_f32(f.z);
        for (int i = 0; i < n; ++i)
        {
            const float32x2_t x = vset_lane_f32(r[i], vdup_n_f32(l[i]), 1);
            y = vadd_f32(vmul_n_f32(x, b), vmul_n_f32(y, a));
            l[i] = vget_lane_f32(y, 0);
            r[i] = vget_lane_f32(y, 1);
        }
        vst1_f32(f.z, y);
        return;
    }
#endif
    float yl = f.z[0], yr = f.z[1];
    for (int i = 0; i < n; ++i)
    {
        yl = b * l[i] + a * yl;
        l[i] = yl;
        yr = b * r[i] + a * yr;
        r[i] = yr;
    }
    f.z[0] = yl;
    f.z[1] = yr;
}

void Distortion::autoGainStage(float* l, float* r) noexcept // FX-1021, scalar and branchy
{
    APP_FX_NOFUSE
    constexpr int n = kBlockFrames;
    float g = g_;
    int hold = hold_;
    const float hi = hi_, lo = lo_, inc = inc_, dec = dec_;
    const int reload = reload_;
    if (hi == 0.0f && g == 1.0f) // Dynamics 0 (off): m >= 0 every sample, g·dec clamps back to 1, L·1 = L
    {
        bool finite = true;
        for (int i = 0; i < n && finite; ++i)
            finite = l[i] == l[i] && r[i] == r[i];
        if (finite)
        {
            hold_ = reload;
            return;
        }
    }
    for (int i = 0; i < n; ++i)
    {
        const float xl = l[i] * g, xr = r[i] * g;
        l[i] = xl;
        r[i] = xr;
        float m = dsp::absf(xl);
        const float mr = dsp::absf(xr);
        if (m < mr)
            m = mr;
        if (m >= hi)
        {
            g = g * dec;
            hold = reload;
            if (g < 1.0f)
                g = 1.0f;
        }
        else if (m >= lo)
            hold = reload;
        else if (hold > 0)
            --hold;
        else
        {
            g = g * inc;
            if (g > kGMax)
                g = kGMax;
        }
    }
    g_ = g;
    hold_ = hold;
}

void Distortion::clipStage(float* x) noexcept
{
    APP_FX_NOFUSE
    constexpr int n = kBlockFrames;
    const float d = drive_, bl = blend_;
#if APP_FX_HAVE_NEON
    if (! scalar_)
    {
        const float32x4_t one = vdupq_n_f32(1.0f), mone = vdupq_n_f32(-1.0f), zero = vdupq_n_f32(0.0f);
        for (int i = 0; i < n; i += 4)
        {
            float32x4_t u = vmulq_n_f32(vld1q_f32(x + i), d);
            u = vminq_f32(vmaxq_f32(u, mone), one);
            const float32x4_t c = vmulq_f32(u, vmulq_f32(u, u));
            const float32x4_t s = vaddq_f32(vmulq_n_f32(c, -0.5f), vmulq_n_f32(u, 1.5f));
            float32x4_t t = vbslq_f32(vcltq_f32(s, zero), vaddq_f32(s, one), vsubq_f32(one, s));
            t = vmulq_f32(t, vmulq_f32(t, t));
            const float32x4_t w = vmulq_n_f32(t, bl);
            const float32x4_t y = vaddq_f32(vmulq_f32(w, c), vmulq_f32(vsubq_f32(one, w), s));
            vst1q_f32(x + i, vmulq_n_f32(y, k07));
        }
        return;
    }
#endif
    for (int i = 0; i < n; ++i)
        x[i] = shape(x[i], d, bl);
}

void Distortion::process(float* left, float* right, const ProcessContext&) noexcept // kernel process 0xfa78c
{
    highPass(left, right);
    lowPass(pre_, left, right);
    autoGainStage(left, right);
    clipStage(left);
    clipStage(right);
    lowPass(out_, left, right);
}
} // namespace engine::fx

APP_REGISTER_EFFECT((engine::fx::EffectDescriptor{
    .id = "distortion",
    .name = "DISTORTION",
    .slots = engine::fx::kAllSlotKinds,
    .params = {"fx.distortion.drive", "fx.distortion.loFreq", "fx.distortion.hiFreq", "fx.distortion.dynamics",
               "fx.distortion.tone", "fx.distortion.mix"},
    .options = {},
    .visual = "none",
    .shell = {.gateMode = static_cast<std::uint8_t>(engine::fx::GateMode::Open),
              .mixLaw = static_cast<std::uint8_t>(engine::fx::MixLaw::CrossfadeK5),
              .wetHighPass = true,
              .skipWhenSilent = false,
              .clampOutput = false},
    .create = &engine::fx::make<engine::fx::Distortion>,
    .featureFlag = {}}))
