// Chorus — see Chorus.h. Float32 steps are single C++ float operations (never fused: APP_FX_NOFUSE, -ffp-contract=off
// on the host, -fno-unsafe-math-optimizations on the device); no libm per sample (the oscillator's set-up only).
// Addresses are the reference's (research docs/effects/chorus.md "1.60 code map"); the bit patterns are the kernel's
// literals (tools/fx/chorus.py).
#include "engine/fx/effects/Chorus.h"

#include "engine/fx/EffectRegistryOrder.h"
#include "engine/fx/FxShell.h"

#include <algorithm>
#include <bit>

#if APP_FX_HAVE_NEON
#include <arm_neon.h>
#endif

namespace engine::fx
{
namespace
{
constexpr int N = kBlockFrames;
const float kRateMin = std::bit_cast<float>(0x3a8888a5u); // 0.00104167 (kernel slot 3 clamp)
constexpr float kRateMax = 64.0f;
const float kTenth = std::bit_cast<float>(0x3dcccccdu);   // 0.1
const float kHundredth = std::bit_cast<float>(0x3c23d70au); // 0.01
const float kMilli = std::bit_cast<float>(0x3a83126fu);   // 0.001
const float kOff = std::bit_cast<float>(0x3e4ccccdu);     // 0.2
const float kPh380 = std::bit_cast<float>(0x3b2c7692u);   // 1/380 (phase degrees → cycles)
const float kPhMax380 = std::bit_cast<float>(0x3ef286bdu); // 180/380
const float kRamp = std::bit_cast<float>(0x3b5844d0u);    // 0.0033
// Sync fractions (a, b): one LFO cycle = a/b beats (16, 8, 4, 2, 1, 3/4, 1/2, 1/4, 1/8, 1/16), chorus.py SYNC_FRACTIONS.
constexpr int kFrac[10][2] = {{16, 1}, {8, 1}, {4, 1}, {2, 1}, {1, 1}, {3, 4}, {1, 2}, {1, 4}, {1, 8}, {1, 16}};

// One interpolated read of FX-122, the model's branches (Kernel.process).
inline float tap(const float* rb, float fw, float d, int ln, float lnf) noexcept
{
    APP_FX_NOFUSE
    float pos = fw - d, fr, om;
    int i0, i1;
    if (pos < 0.0f)
    {
        pos = pos + lnf;
        i0 = static_cast<int>(pos);
        if (i0 < ln)
        {
            fr = pos - static_cast<float>(i0);
            om = 1.0f - fr;
            i1 = i0 + 1;
        }
        else
        {
            fr = 0.0f;
            om = 1.0f;
            i0 = 0;
            i1 = 1;
        }
    }
    else
    {
        i0 = static_cast<int>(pos);
        fr = pos - static_cast<float>(i0);
        om = 1.0f - fr;
        i1 = i0 + 1;
    }
    if (i1 >= ln)
        i1 = 0;
    return fr * rb[i1] + om * rb[i0];
}
} // namespace

int Chorus::kernelRateFor(std::uint32_t quirks) const noexcept
{
    return (quirks & kChorusQuirkFixed44k) != 0 ? 44100 : static_cast<int>(fs_); // FX-182
}

void Chorus::prepare(double sampleRate, int)
{
    fs_ = sampleRate;
    cap_ = std::max(static_cast<int>(sampleRate) / 10, 44100 / 10) + N;
    ring_.assign(static_cast<std::size_t>(2 * cap_), 0.0f); // rt-ok: prepare, message thread
    storedMask_ = 0;
    build();
}

void Chorus::zeroRings() noexcept
{
    std::fill(ring_.begin(), ring_.end(), 0.0f);
}

void Chorus::build() noexcept // c_chorus ctor 0xff008 + the Chorus fields (InsertFxProcessor ctor 0x118b0c)
{
    APP_FX_NOFUSE
    built_ = quirks_;
    const bool fixed = (built_ & kChorusQuirkFixed44k) != 0;
    kfs_ = kernelRateFor(built_);
    ln_ = kfs_ / 10 + N;
    osc_ = ModOscillator{};
    osc_.setQuirkProfile(fixed ? kOscQuirkPeriod44k : 0u);
    osc_.init(ModOscillator::kSine, 1.0f, fixed ? 44100.0 : static_cast<double>(kfs_)); // the literal 44100
    const float fsf = static_cast<float>(kfs_);
    const float k = fsf * kRamp;
    w_ = 0;
    const float q = static_cast<float>(N) / k;
    coef_ = 1.0f / k;
    tol_ = q * (q * (q * (q * q)));
    retrig_ = false;
    resetFlag_ = true;
    g_ = target_ = step_ = 0.0f;
    full_ = false;
    rate_ = kTenth;
    depth_ = 100.0f;
    phase_ = 0.0f;
    scale_ = 1.0f;
    osc_.setFrequency(kTenth);
    const float amp = (fsf * kMilli) * 0.5f;
    osc_.setAmplitude(amp);
    osc_.setOffset(amp + kOff);
    osc_.setPhaseOffset(0.0f);
    std::fill(std::begin(dL_), std::end(dL_), 0.0f);
    std::fill(std::begin(dR_), std::end(dR_), 0.0f);
    zeroRings();
    pRate_.init(0.5f);
    pSync_.init(0);
    pBpm_.init(120.0f);
    rateV_ = 0.5f;
    syncV_ = 0;
    bpmV_ = 120.0f;
    dirty_ = (built_ & kChorusQuirkChangeTrap) == 0; // FX-056: Sixteen applies the first rate whatever it is
    fresh_ = true;
    for (int i = 0; i < 6; ++i)
        if ((storedMask_ >> i) & 1u)
            apply(i, stored_[i]);
    if ((storedMask_ >> 6) & 1u)
        pSync_.set(static_cast<float>(storedSync_));
}

void Chorus::setRate(float hz) noexcept // kernel slot 3 0xfeb10
{
    float mag;
    const float arg = dsp::mod::clampRate(hz, kRateMin, kRateMax, mag);
    rate_ = mag;
    if (ln_ == 0 || ! osc_.setFrequency(arg))
        rate_ = 0.0f;
}

void Chorus::applyAmp() noexcept
{
    APP_FX_NOFUSE
    const float t = (depth_ * scale_) * kHundredth;
    if (t < 0.0f || ! (t <= 100.0f))
        return;
    const float amp = ((t * static_cast<float>(kfs_)) * kMilli) * 0.5f;
    osc_.setAmplitude(amp);
    osc_.setOffset(amp + kOff);
}

void Chorus::setDepth(float v) noexcept // kernel slot 5 0xfed90, grid [0, 101)
{
    float d = static_cast<float>(dsp::mod::rintIdx(0, 101, v));
    d = d < 0.0f ? 0.0f : (d > 100.0f ? 100.0f : d);
    depth_ = d;
    if (ln_ != 0)
        applyAmp();
}

void Chorus::setScale(float v) noexcept // kernel slot 7 0xfeecc, grid [10, 101) × 0.1
{
    APP_FX_NOFUSE
    float s = static_cast<float>(dsp::mod::rintIdx(10, 101, v)) * kTenth;
    s = s < 1.0f ? 1.0f : (s > 100.0f ? 100.0f : s);
    scale_ = s;
    if (ln_ != 0)
        applyAmp();
}

void Chorus::setPhase(float v) noexcept // kernel slot 6 0xfec34, grid [0, 181) degrees
{
    APP_FX_NOFUSE
    const bool q380 = (quirks_ & kChorusQuirkPhase380) != 0; // FX-181
    float deg = static_cast<float>(dsp::mod::rintIdx(0, 181, v)), ph;
    if (deg < 0.0f)
        deg = ph = 0.0f;
    else if (deg > 180.0f)
    {
        deg = 180.0f;
        ph = q380 ? kPhMax380 : 0.5f;
    }
    else
        ph = q380 ? deg * kPh380 : deg / 360.0f;
    phase_ = deg;
    if (ln_ == 0 || ! osc_.setPhaseOffset(ph))
        phase_ = 0.0f;
}

void Chorus::apply(int k, float v) noexcept // Chorus slot 14 0x116224
{
    switch (k)
    {
    case 0:
        pRate_.set(v); // deferred to the next block (FX-160)
        break;
    case 1:
        setDepth(v);
        break;
    case 2:
        setPhase(v);
        break;
    case 3:
        setScale(v);
        break;
    default: // 4 unused, 5 mix: the shell's (FX-124)
        break;
    }
}

void Chorus::setParam(int index, float value01) noexcept
{
    if (index < 0 || index >= 6)
        return;
    stored_[index] = value01;
    storedMask_ |= 1u << index;
    apply(index, value01);
}

void Chorus::setOption(int which, int value) noexcept
{
    if (which == kOptSync)
    {
        storedSync_ = value;
        storedMask_ |= 1u << 6;
        pSync_.set(static_cast<float>(value));
    }
    else if (which == kRetrigger && value != 0)
        retrigger();
    else if (which == kClearState && value != 0)
        build();
}

void Chorus::reset() noexcept // slot 27 0x115a14 → kernel slot 2: zero the rings and the LFO buffers (FX-161)
{
    // The profile is honoured here (FX-053): a kernel never run is rebuilt as constructed under it; a running one
    // only when its kernel rate changes (FX-182 at fs != 44.1 kHz).
    if (fresh_ || kernelRateFor(quirks_) != kfs_)
        build();
    osc_.setQuirkProfile((quirks_ & kChorusQuirkFixed44k) != 0 ? kOscQuirkPeriod44k : 0u);
    zeroRings();
    std::fill(std::begin(dL_), std::end(dL_), 0.0f);
    std::fill(std::begin(dR_), std::end(dR_), 0.0f);
}

void Chorus::delayScalar(float* l, float* r) noexcept
{
    APP_FX_NOFUSE
    const int ln = ln_;
    const float lnf = static_cast<float>(ln);
    float* rb0 = ring_.data();
    float* rb1 = rb0 + cap_;
    int w = w_;
    for (int i = 0; i < N; ++i)
    {
        const float fw = static_cast<float>(w);
        rb0[w] = l[i];
        rb1[w] = r[i];
        l[i] = tap(rb0, fw, dL_[i], ln, lnf);
        r[i] = tap(rb1, fw, dR_[i], ln, lnf);
        if (++w >= ln)
        {
            w = 0;
            full_ = true;
        }
    }
    w_ = w;
}

void Chorus::delayNeon(float* l, float* r) noexcept
{
#if APP_FX_HAVE_NEON
    const int ln = ln_;
    float* rb0 = ring_.data();
    float* rb1 = rb0 + cap_;
    const float32x4_t lnf = vdupq_n_f32(static_cast<float>(ln)), zero = vdupq_n_f32(0.0f), one = vdupq_n_f32(1.0f);
    const int32x4_t lnv = vdupq_n_s32(ln), zi = vdupq_n_s32(0), onei = vdupq_n_s32(1);
    alignas(16) std::int32_t wi[4], a0[4], a1[4];
    int w = w_;
    for (int i = 0; i < N; i += 4)
    {
        for (int j = 0; j < 4; ++j) // the ring writes, scalar
        {
            wi[j] = w;
            rb0[w] = l[i + j];
            rb1[w] = r[i + j];
            if (++w >= ln)
            {
                w = 0;
                full_ = true;
            }
        }
        const float32x4_t fw = vcvtq_f32_s32(vld1q_s32(wi));
        for (int ch = 0; ch < 2; ++ch)
        {
            const float* rb = ch == 0 ? rb0 : rb1;
            float* out = (ch == 0 ? l : r) + i;
            float32x4_t pos = vsubq_f32(fw, vld1q_f32((ch == 0 ? dL_ : dR_) + i));
            pos = vbslq_f32(vcltq_f32(pos, zero), vaddq_f32(pos, lnf), pos);
            int32x4_t i0 = vcvtq_s32_f32(pos); // truncation, as the reference's vcvt
            const uint32x4_t big = vcgeq_s32(i0, lnv);
            const float32x4_t fr = vbslq_f32(big, zero, vsubq_f32(pos, vcvtq_f32_s32(i0)));
            i0 = vbslq_s32(big, zi, i0);
            const float32x4_t om = vsubq_f32(one, fr);
            int32x4_t i1 = vaddq_s32(i0, onei);
            i1 = vbslq_s32(vcgeq_s32(i1, lnv), zi, i1);
            vst1q_s32(a0, i0);
            vst1q_s32(a1, i1);
            float32x4_t t0 = zero, t1 = zero;
            t0 = vld1q_lane_f32(rb + a0[0], t0, 0);
            t0 = vld1q_lane_f32(rb + a0[1], t0, 1);
            t0 = vld1q_lane_f32(rb + a0[2], t0, 2);
            t0 = vld1q_lane_f32(rb + a0[3], t0, 3);
            t1 = vld1q_lane_f32(rb + a1[0], t1, 0);
            t1 = vld1q_lane_f32(rb + a1[1], t1, 1);
            t1 = vld1q_lane_f32(rb + a1[2], t1, 2);
            t1 = vld1q_lane_f32(rb + a1[3], t1, 3);
            vst1q_f32(out, vaddq_f32(vmulq_f32(fr, t1), vmulq_f32(om, t0)));
        }
    }
    w_ = w;
#else
    delayScalar(l, r);
#endif
}

void Chorus::gainStage(float* l, float* r) noexcept // the output gain of slot 19 (FX-123, FX-180)
{
    APP_FX_NOFUSE
    if ((quirks_ & kChorusQuirkStartMute) == 0)
        return; // Sixteen: g_out = 1 (1·y = y)
    float g = g_;
    target_ = full_ ? 1.0f : 0.0f;
    const float step = (target_ - g) * coef_;
    step_ = step;
    for (int i = 0; i < N; ++i)
    {
        g = g + step;
        l[i] = g * l[i];
        r[i] = r[i] * g;
    }
    g_ = g;
    if (step > 0.0f && target_ < g + tol_)
        g_ = target_;
    if (step < 0.0f && g - tol_ < target_)
        g_ = target_;
}

void Chorus::process(float* left, float* right, const ProcessContext& ctx) noexcept // slot 25 0x115d70
{
    APP_FX_NOFUSE
    fresh_ = false;
    pBpm_.set(static_cast<float>(ctx.bpm)); // FX-049: the tempo every block (the reference's BPM index 7)
    if (pRate_.changed())
    {
        rateV_ = pRate_.get();
        dirty_ = true;
    }
    if (pSync_.changed())
    {
        syncV_ = pSync_.get();
        dirty_ = true;
    }
    if (pBpm_.changed())
    {
        bpmV_ = pBpm_.get();
        dirty_ = true;
    }
    if (dirty_)
    {
        float hz;
        if (syncV_ != 0)
        {
            int i = dsp::mod::rintIdx(0, 10, rateV_);
            if (i < 0 || i > 9)
                i = 2; // the reference's assert path
            hz = dsp::mod::syncHz(kFrac[i][0], kFrac[i][1], bpmV_);
        }
        else
            hz = static_cast<float>(dsp::mod::rintIdx(1, 101, rateV_)) * kTenth;
        setRate(hz);
        dirty_ = false;
    }
    // kernel slot 19 0xfe710
    if (resetFlag_)
    {
        w_ = 0;
        zeroRings();
        full_ = false;
        osc_.resetPhase(0.0f);
        resetFlag_ = false;
    }
    if (retrig_)
    {
        osc_.resetPhase(0.0f);
        retrig_ = false;
    }
    osc_.process(dL_, dR_, N);
    if (scalar_)
        delayScalar(left, right);
    else
        delayNeon(left, right);
    gainStage(left, right);
}
} // namespace engine::fx

APP_REGISTER_EFFECT((engine::fx::EffectDescriptor{
    .id = "chorus",
    .name = "CHORUS",
    .slots = engine::fx::kAllSlotKinds,
    .params = {"fx.chorus.rate", "fx.chorus.width", "fx.chorus.phase", "fx.chorus.delay", "", "fx.chorus.mix"},
    .options = engine::fx::kChorusOptions,
    .visual = "lfoCurve",
    .shell = {.gateMode = static_cast<std::uint8_t>(engine::fx::GateMode::Open),
              .mixLaw = static_cast<std::uint8_t>(engine::fx::MixLaw::UnityDryK5),
              .wetHighPass = true,
              .skipWhenSilent = false,
              .clampOutput = false},
    .create = &engine::fx::make<engine::fx::Chorus>,
    .featureFlag = {}}))
