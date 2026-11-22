// Phaser — see Phaser.h. Float32 steps are single C++ float operations (never fused); libm (tan, pow, exp) only in the
// set-up paths (activation, parameter changes), never per sample. Addresses are the reference's (research
// docs/effects/phaser.md "1.60 code map"); the constants are the model's literals (tools/fx/phaser.py).
#include "engine/fx/effects/Phaser.h"

#include "engine/fx/EffectRegistryOrder.h"
#include "engine/fx/FxShell.h"
#include "engine/fx/dsp/filter_fdlibm.h"
#include "engine/fx/dsp/phaser_tanf.h"

#include <bit>
#include <cmath>

#if APP_FX_HAVE_NEON
#include <arm_neon.h>
#endif

namespace engine::fx
{
namespace
{
constexpr int N = kBlockFrames;
const float kPiF = std::bit_cast<float>(0x40490fdbu); // f32(π)
const float kLo = 314.15927f;                         // π·100 (ctor literal)
const float kHi = 62831.855f;                         // π·20000 (ctor literal)
constexpr double kFbLp = -62831.853071795864;         // −2π·10000 (double literal): pole = exp(kFbLp / fs)
constexpr float kCtor[6] = {0.5f, 1.0f, 0.0f, 0.0f, 0.5f, 1.0f}; // the constructor's "last" values (CTOR_VALUES)
// Sync divisions (beats per LFO cycle = num/den): 64, 32, 16, 8, 4, 2, 1, 3/4, 1/2, 1/4, 1/8, 1/16.
constexpr int kFrac[12][2] = {{64, 1}, {32, 1}, {16, 1}, {8, 1}, {4, 1}, {2, 1},
                              {1, 1},  {3, 4},  {1, 2},  {1, 4}, {1, 8}, {1, 16}};

float rateHz(float v) noexcept // free Rate 0x34c220/0x34c240: rint(1 + 999v)·0.01
{
    APP_FX_NOFUSE
    return static_cast<float>(dsp::mod::rintIdx(1, 1001, v)) * 0.01f;
}

int syncIndex(float v) noexcept // sync Rate 0x34c28c/0x34c2a8
{
    const int i = dsp::mod::rintIdx(0, 12, v);
    return i >= 0 && i <= 11 ? i : 4;
}
} // namespace

float Phaser::frequencyHz(float v, bool parity) noexcept // Frequency 0x34c8a8/0x34c8c8
{
    APP_FX_NOFUSE
    const int idx = dsp::mod::rintIdx(0, 257, v);
    const float e = static_cast<float>(idx) * 0.00390625f;
    const float p = parity ? dsp::fdlibm::powf(60.0f, e)
                           : static_cast<float>(std::pow(60.0, static_cast<double>(e)));
    return p * 300.0f;
}

float Phaser::tanOf(float x) const noexcept
{
    return parity_ ? dsp::fdlibm::tanf(x) : static_cast<float>(std::tan(static_cast<double>(x)));
}

void Phaser::initLfo(float hz) noexcept // 0x100b28 → 0x100710, then the Phaser's oscillator set-up
{
    lfo_ = ModOscillator{};
    lfo_.setQuirkProfile((quirks_ & kPhaserQuirkPeriod44k) != 0 ? kOscQuirkPeriod44k : 0u);
    lfo_.init(ModOscillator::kTriangle, hz, fs_);
    lfo_.resetPhase(0.25f);
    lfo_.setOffset(0.5f);
    lfo_.setAmplitude(0.5f);
    lfo_.setSmoothingOff(true);
}

void Phaser::prepare(double sampleRate, int)
{
    fs_ = sampleRate;
    storedMask_ = 0;
    build();
}

void Phaser::build() noexcept // ctor 0x10d654
{
    APP_FX_NOFUSE
    fsf_ = static_cast<float>(fs_);
    rc_ = 3 * N;
    tLo_ = tanOf(kLo / fsf_);
    tHi_ = tanOf(kHi / fsf_);
    aLo_ = (tLo_ - 1.0f) / (tLo_ + 1.0f);
    aHi_ = (tHi_ - 1.0f) / (tHi_ + 1.0f);
    gHi_ = 0.5f;
    gLo_ = 0.7f;
    for (int k = 0; k < 6; ++k)
        p_[k].init(kCtor[k]);
    pSync_.init(0);
    pBpm_.init(120.0f);
    cntA_ = cntF_ = -1;
    fb_ = 0.0f;
    y_[0] = y_[1] = 0.0f;
    a_[0] = a_[1] = aHi_;
    aT_[0] = aT_[1] = aHi_;
    for (auto& z : z_)
        z[0] = z[1] = 0.0f;
    pole_ = static_cast<float>(std::exp(kFbLp / fs_));
    pole1_ = 1.0f - pole_;
    s_[0] = s_[1] = 0.0f;
    loT_ = tLo_;
    hiT_ = tHi_;
    initLfo(1.0f);
    retrig_ = false;
    // ctor tail: the per-block apply on the constructor values (rate, phase, range, feedback)
    updateRate(true);
    lfo_.setPhaseOffset(p_[2].get() * 0.5f);
    updateRange(false);
    fbT_ = 0.0f + p_[4].get() * 0.99f;
    fb_ = fbT_;
    fresh_ = true;
    for (int k = 0; k < 6; ++k)
        if ((storedMask_ >> k) & 1u)
            p_[k].set(stored_[k]);
    if ((storedMask_ >> 6) & 1u)
        pSync_.set(static_cast<float>(storedSync_));
}

void Phaser::updateRate(bool apply) noexcept // 0x10df00
{
    APP_FX_NOFUSE
    const float v = p_[0].get();
    float ms;
    if (pSync_.get() == 0)
        ms = 1000.0f / rateHz(v);
    else
    {
        const auto& fr = kFrac[syncIndex(v)];
        ms = ((60000.0f / pBpm_.get()) * static_cast<float>(fr[0])) / static_cast<float>(fr[1]);
    }
    periodMs_ = ms;
    if (apply)
        lfo_.setPeriodMs(ms);
}

void Phaser::updateRange(bool setCount) noexcept // inline in 0x10d260 (also the ctor and slot 27)
{
    APP_FX_NOFUSE
    const float width = p_[1].get();
    const float f = frequencyHz(p_[3].get(), parity_);
    const float hi = tanOf((f * kPiF) / fsf_);
    hiT_ = tHi_ < hi ? tHi_ : hi;
    const float u = 1.0f - width;
    const float loF = f - (1.0f - u * u) * (f - 100.0f);
    const float lo = tanOf((loF * kPiF) / fsf_);
    if (setCount)
        cntA_ = rc_;
    loT_ = tLo_ > lo ? tLo_ : lo;
}

float Phaser::coefOf(float u) const noexcept
{
    APP_FX_NOFUSE
    const float rng = hiT_ - loT_;
    const float w = loT_ + (u * u) * u * rng;
    return (w - 1.0f) / (w + 1.0f);
}

void Phaser::setParam(int index, float value01) noexcept // EffectBase slot 14 0x105d24
{
    if (index < 0 || index >= 6)
        return;
    stored_[index] = value01;
    storedMask_ |= 1u << index;
    p_[index].set(value01);
}

void Phaser::setOption(int which, int value) noexcept
{
    if (which == kOptSync)
    {
        storedSync_ = value;
        storedMask_ |= 1u << 6;
        pSync_.set(static_cast<float>(value));
    }
    else if (which == kOptParityMaths)
        parity_ = value != 0;
    else if (which == kRetrigger && value != 0)
        retrigger();
    else if (which == kClearState && value != 0)
        build();
}

void Phaser::reset() noexcept // slot 27 0x10cf30 → 0x10cb78 (FX-361); the change flags are not consumed
{
    APP_FX_NOFUSE
    if (fresh_)
        build(); // a never-run instance: as constructed under the current profile and maths
    cntA_ = -1;
    updateRate(false);
    const float ms = periodMs_;
    if (ms != 0.0f)
        initLfo(1000.0f / ms);
    else
    {
        lfo_.resetPhase(0.25f);
        lfo_.setOffset(0.5f);
        lfo_.setAmplitude(0.5f);
        lfo_.setSmoothingOff(true);
    }
    lfo_.setPhaseOffset(p_[2].get() * 0.5f);
    updateRange(false);
    cntF_ = -1;
    y_[0] = y_[1] = 0.0f;
    fbT_ = 0.0f + p_[4].get() * 0.99f;
    fb_ = fbT_;
    for (auto& z : z_)
        z[0] = z[1] = 0.0f;
    s_[0] = s_[1] = 0.0f;
    float l1, r1;
    lfo_.process(&l1, &r1, 1); // one sample seeds a
    a_[0] = coefOf(l1);
    a_[1] = coefOf(r1);
    retrig_ = false;
}

void Phaser::kernelScalar(float* l, float* r, float dfb, const float* da) noexcept // 0x10cf40, per channel
{
    APP_FX_NOFUSE
    const float inv = 1.0f / (aHi_ - aLo_), diff = gLo_ - gHi_;
    const float pole = pole_, pole1 = pole1_, aLo = aLo_, gLo = gLo_;
    float fb = fb_;
    float* buf[2] = {l, r};
    for (int i = 0; i < N; ++i)
    {
        fb = fb + dfb;
        for (int j = 0; j < 2; ++j)
        {
            const float t1 = y_[j] * pole1, t2 = s_[j] * pole;
            const float a = a_[j] + da[j];
            a_[j] = a;
            s_[j] = t1 + t2;
            const float xin = buf[j][i];
            float x = s_[j] * fb + xin;
            const float g = gLo - ((a - aLo) * inv) * diff;
            for (auto& zk : z_)
            {
                const float v = x - a * zk[j];
                x = a * v + zk[j];
                zk[j] = v;
            }
            const float out = (xin + x) * g;
            buf[j][i] = out;
            y_[j] = out;
        }
    }
    fb_ = fb;
}

void Phaser::kernelNeon(float* l, float* r, float dfb, const float* da) noexcept // 0x10cf40, L/R lanes
{
#if APP_FX_HAVE_NEON
    const float32x2_t inv = vdup_n_f32(1.0f / (aHi_ - aLo_)), diff = vdup_n_f32(gLo_ - gHi_);
    const float32x2_t pole = vdup_n_f32(pole_), pole1 = vdup_n_f32(pole1_), aLo = vdup_n_f32(aLo_),
                      gLo = vdup_n_f32(gLo_), dfbv = vdup_n_f32(dfb), dav = vld1_f32(da);
    float32x2_t fb = vdup_n_f32(fb_), y = vld1_f32(y_), s = vld1_f32(s_), a = vld1_f32(a_);
    float32x2_t z0 = vld1_f32(z_[0]), z1 = vld1_f32(z_[1]), z2 = vld1_f32(z_[2]), z3 = vld1_f32(z_[3]),
                z4 = vld1_f32(z_[4]), z5 = vld1_f32(z_[5]);
    for (int i = 0; i < N; ++i)
    {
        fb = vadd_f32(fb, dfbv);
        const float32x2_t t1 = vmul_f32(y, pole1), t2 = vmul_f32(s, pole);
        a = vadd_f32(a, dav);
        s = vadd_f32(t1, t2);
        float32x2_t xin = vdup_n_f32(0.0f);
        xin = vld1_lane_f32(l + i, xin, 0);
        xin = vld1_lane_f32(r + i, xin, 1);
        float32x2_t x = vadd_f32(vmul_f32(s, fb), xin);
        const float32x2_t g = vsub_f32(gLo, vmul_f32(vmul_f32(vsub_f32(a, aLo), inv), diff));
        float32x2_t v;
        v = vsub_f32(x, vmul_f32(a, z0));
        x = vadd_f32(vmul_f32(a, v), z0);
        z0 = v;
        v = vsub_f32(x, vmul_f32(a, z1));
        x = vadd_f32(vmul_f32(a, v), z1);
        z1 = v;
        v = vsub_f32(x, vmul_f32(a, z2));
        x = vadd_f32(vmul_f32(a, v), z2);
        z2 = v;
        v = vsub_f32(x, vmul_f32(a, z3));
        x = vadd_f32(vmul_f32(a, v), z3);
        z3 = v;
        v = vsub_f32(x, vmul_f32(a, z4));
        x = vadd_f32(vmul_f32(a, v), z4);
        z4 = v;
        v = vsub_f32(x, vmul_f32(a, z5));
        x = vadd_f32(vmul_f32(a, v), z5);
        z5 = v;
        y = vmul_f32(vadd_f32(xin, x), g);
        vst1_lane_f32(l + i, y, 0);
        vst1_lane_f32(r + i, y, 1);
    }
    fb_ = vget_lane_f32(fb, 0);
    vst1_f32(y_, y);
    vst1_f32(s_, s);
    vst1_f32(a_, a);
    vst1_f32(z_[0], z0);
    vst1_f32(z_[1], z1);
    vst1_f32(z_[2], z2);
    vst1_f32(z_[3], z3);
    vst1_f32(z_[4], z4);
    vst1_f32(z_[5], z5);
#else
    kernelScalar(l, r, dfb, da);
#endif
}

void Phaser::process(float* left, float* right, const ProcessContext& ctx) noexcept // slot 25 0x10d630 → 0x10d260
{
    APP_FX_NOFUSE
    fresh_ = false;
    pBpm_.set(static_cast<float>(ctx.bpm)); // FX-049: the reference's BPM index 7, every block
    if (retrig_)
    {
        lfo_.resetPhase(0.25f); // FX-360: at the peak
        retrig_ = false;
    }
    if (p_[0].changed() || pBpm_.changed() || pSync_.changed()) // the reference's short-circuit order
    {
        updateRate(true);
        cntA_ = rc_;
    }
    if (p_[2].changed())
    {
        lfo_.setPhaseOffset(p_[2].get() * 0.5f);
        cntA_ = rc_;
    }
    lfo_.process(lL_, lR_, N);
    if (p_[3].changed() || p_[1].changed())
        updateRange(true);
    {
        const float lo = loT_, rng = hiT_ - lo;
        const float xr = lR_[N - 1], xl = lL_[N - 1];
        const float r3 = (xr * xr) * xr, l3 = (xl * xl) * xl;
        const float wR = lo + rng * r3, wL = lo + l3 * rng;
        aT_[1] = (wR - 1.0f) / (wR + 1.0f);
        aT_[0] = (wL - 1.0f) / (wL + 1.0f);
    }
    const float div = cntA_ > 0 ? static_cast<float>(cntA_) : static_cast<float>(N);
    const float da[2] = {(aT_[0] - a_[0]) / div, (aT_[1] - a_[1]) / div};
    if (p_[4].changed())
    {
        fbT_ = 0.0f + p_[4].get() * 0.99f;
        cntF_ = rc_;
    }
    const float dfb = cntF_ > 0 ? (fbT_ - fb_) / static_cast<float>(cntF_) : 0.0f;
    if (scalar_)
        kernelScalar(left, right, dfb, da);
    else
        kernelNeon(left, right, dfb, da);
    if (cntA_ > 0)
    {
        cntA_ -= N;
        if (cntA_ <= 0)
        {
            a_[0] = aT_[0];
            a_[1] = aT_[1];
        }
    }
    if (cntF_ > 0)
    {
        cntF_ -= N;
        if (cntF_ <= 0)
            fb_ = fbT_;
    }
}
} // namespace engine::fx

APP_REGISTER_EFFECT((engine::fx::EffectDescriptor{
    .id = "phaser",
    .name = "PHASER",
    .slots = engine::fx::kAllSlotKinds,
    .params = {"fx.phaser.rate", "fx.phaser.width", "fx.phaser.phase", "fx.phaser.frequency", "fx.phaser.feedback",
               "fx.phaser.mix"},
    .options = engine::fx::kPhaserOptions,
    .visual = "lfoCurve",
    .shell = {.gateMode = static_cast<std::uint8_t>(engine::fx::GateMode::Open),
              .mixLaw = static_cast<std::uint8_t>(engine::fx::MixLaw::CrossfadeK5),
              .wetHighPass = true,
              .skipWhenSilent = false,
              .clampOutput = false},
    .create = &engine::fx::make<engine::fx::Phaser>,
    .featureFlag = {}}))
