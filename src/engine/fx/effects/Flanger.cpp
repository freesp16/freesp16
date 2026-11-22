// Flanger — see Flanger.h. Float32 steps are single C++ float operations (never fused); no libm per sample.
// Addresses are the reference's (research docs/effects/flanger.md "1.60 code map"); the bit patterns are the kernel's
// literals (tools/fx/flanger.py).
#include "engine/fx/effects/Flanger.h"

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
const float kRateMin = std::bit_cast<float>(0x3988888au); // 0.00026041671 (kernel slot 3 clamp)
constexpr float kRateMax = 128.0f;
const float kTenth = std::bit_cast<float>(0x3dcccccdu);
const float kHundredth = std::bit_cast<float>(0x3c23d70au);
const float kMilli = std::bit_cast<float>(0x3a83126fu);
const float kOff = std::bit_cast<float>(0x3e4ccccdu);     // 0.2
const float kPh380 = std::bit_cast<float>(0x3b2c7692u);   // 1/380
const float kPhMax380 = std::bit_cast<float>(0x3ef286bdu); // 180/380
const float kFbMax = std::bit_cast<float>(0x3f7d70a3u);   // 0.98999995
const float kKnee = 48.0f;                                // 0x42400000
const float kGSlope = std::bit_cast<float>(0x3c578493u);  // 0.013154167
const float kGSub = std::bit_cast<float>(0x3f21a36eu);    // 0.6314
// Sync fractions (a, b): one LFO cycle = a/b beats (64, 32, 16, 8, 4, 2, 1), flanger.py SYNC_FRACTIONS.
constexpr int kFrac[7][2] = {{64, 1}, {32, 1}, {16, 1}, {8, 1}, {4, 1}, {2, 1}, {1, 1}};
} // namespace

float Flanger::delayGain(float d) noexcept
{
    APP_FX_NOFUSE
    return d <= kKnee ? (1.0f + d * kGSlope) - kGSub : 1.0f;
}

int Flanger::kernelRateFor(std::uint32_t quirks) const noexcept
{
    return (quirks & kFlangerQuirkFixed44k) != 0 ? 44100 : static_cast<int>(fs_); // FX-280
}

void Flanger::prepare(double sampleRate, int)
{
    fs_ = sampleRate;
    cap_ = std::max(static_cast<int>(sampleRate) * 7 / 1000, 44100 * 7 / 1000) + N;
    ring_.assign(static_cast<std::size_t>(2 * cap_), 0.0f); // rt-ok: prepare, message thread
    storedMask_ = 0;
    build();
}

void Flanger::zeroBuffers() noexcept // kernel slot 2 0xff990 → 0xff798: rings, oscillator and gain buffers
{
    std::fill(ring_.begin(), ring_.end(), 0.0f);
    std::fill(std::begin(dL_), std::end(dL_), 0.0f);
    std::fill(std::begin(dR_), std::end(dR_), 0.0f);
    std::fill(std::begin(gL_), std::end(gL_), 0.0f);
    std::fill(std::begin(gR_), std::end(gR_), 0.0f);
}

void Flanger::build() noexcept // c_flanger ctor 0x100234 + the Flanger fields
{
    APP_FX_NOFUSE
    const bool fixed = (quirks_ & kFlangerQuirkFixed44k) != 0;
    kfs_ = kernelRateFor(quirks_);
    ln_ = kfs_ * 7 / 1000 + N;
    osc_ = ModOscillator{}; // FX-282: a zeroed oscillator (counter 0)
    osc_.setQuirkProfile(fixed ? kOscQuirkPeriod44k : 0u);
    osc_.init(ModOscillator::kSine, 1.0f, fixed ? 44100.0 : static_cast<double>(kfs_)); // the literal 44100
    const float fsf = static_cast<float>(kfs_);
    w_ = 0;
    retrig_ = false;
    resetFlag_ = true;
    y_[0] = y_[1] = 0.0f;
    fb_ = 0.0f;
    zeroBuffers();
    rate_ = kTenth;
    depth_ = 100.0f;
    phase_ = 0.0f;
    scale_ = 2.0f;
    fbI_ = 0.0f;
    osc_.setFrequency(kTenth);
    const float amp = ((fsf + fsf) * kMilli) * 0.5f;
    osc_.setAmplitude(amp);
    osc_.setOffset(amp + kOff);
    pRate_.init(0.5f);
    pSync_.init(0);
    pBpm_.init(120.0f);
    rateV_ = 0.5f;
    syncV_ = 0;
    bpmV_ = 120.0f;
    dirty_ = (quirks_ & kFlangerQuirkChangeTrap) == 0; // FX-056
    fresh_ = true;
    for (int i = 0; i < 6; ++i)
        if ((storedMask_ >> i) & 1u)
            apply(i, stored_[i]);
    if ((storedMask_ >> 6) & 1u)
        pSync_.set(static_cast<float>(storedSync_));
}

void Flanger::setRate(float hz) noexcept // kernel slot 3 0xffdc4
{
    float mag;
    const float arg = dsp::mod::clampRate(hz, kRateMin, kRateMax, mag);
    rate_ = mag;
    if (ln_ == 0 || ! osc_.setFrequency(arg))
        rate_ = 0.0f;
}

void Flanger::applyAmp() noexcept
{
    APP_FX_NOFUSE
    float t = (depth_ * scale_) * kHundredth;
    if (t < 0.0f)
        t = 0.0f;
    const float amp = ((static_cast<float>(kfs_) * t) * kMilli) * 0.5f;
    osc_.setAmplitude(amp);
    osc_.setOffset(amp + kOff);
}

void Flanger::setDepth(float v) noexcept // kernel slot 5 0xfffd4, grid [0, 101)
{
    float d = static_cast<float>(dsp::mod::rintIdx(0, 101, v));
    d = d < 0.0f ? 0.0f : (d > 100.0f ? 100.0f : d);
    depth_ = d;
    if (ln_ != 0)
        applyAmp();
}

void Flanger::setScale(float v) noexcept // kernel slot 7 0x100104, grid [1, 71) × 0.1
{
    APP_FX_NOFUSE
    float s = static_cast<float>(dsp::mod::rintIdx(1, 71, v)) * kTenth;
    s = s < kTenth ? kTenth : (s > 7.0f ? 7.0f : s);
    scale_ = s;
    if (ln_ != 0)
        applyAmp();
}

void Flanger::setPhase(float v) noexcept // kernel slot 6 0xffe68, grid [0, 181) degrees
{
    APP_FX_NOFUSE
    const bool q380 = (quirks_ & kFlangerQuirkPhase380) != 0; // FX-281
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

void Flanger::setFeedback(float v) noexcept // kernel slot 8 0xff694, grid [0, 100)
{
    APP_FX_NOFUSE
    float i = static_cast<float>(dsp::mod::rintIdx(0, 100, v)), f;
    if (i < 0.0f)
        i = f = 0.0f;
    else if (i <= 99.0f)
        f = i * kHundredth;
    else
    {
        i = 99.0f;
        f = kFbMax;
    }
    fbI_ = i;
    if (ln_ == 0 || ! (f >= 0.0f) || ! (f < 1.0f))
    {
        fbI_ = 0.0f;
        return;
    }
    fb_ = f;
}

void Flanger::apply(int k, float v) noexcept // Flanger slot 14 0x1163f0
{
    switch (k)
    {
    case 0:
        pRate_.set(v);
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
    case 4:
        setFeedback(v);
        break;
    default: // 5 mix: the shell's (FX-224)
        break;
    }
}

void Flanger::setParam(int index, float value01) noexcept
{
    if (index < 0 || index >= 6)
        return;
    stored_[index] = value01;
    storedMask_ |= 1u << index;
    apply(index, value01);
}

void Flanger::setOption(int which, int value) noexcept
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

void Flanger::reset() noexcept // slot 27 0x1159b0 → kernel slot 2 (FX-261)
{
    if (fresh_ || kernelRateFor(quirks_) != kfs_)
        build();
    osc_.setQuirkProfile((quirks_ & kFlangerQuirkFixed44k) != 0 ? kOscQuirkPeriod44k : 0u);
    zeroBuffers();
}

void Flanger::prepass() noexcept
{
    APP_FX_NOFUSE
#if APP_FX_HAVE_NEON
    if (! scalar_)
    {
        const float32x4_t knee = vdupq_n_f32(kKnee), one = vdupq_n_f32(1.0f), slope = vdupq_n_f32(kGSlope),
                          sub = vdupq_n_f32(kGSub);
        for (int ch = 0; ch < 2; ++ch)
        {
            const float* d = ch == 0 ? dL_ : dR_;
            float* g = ch == 0 ? gL_ : gR_;
            float* fr = ch == 0 ? frL_ : frR_;
            std::int32_t* ix = ch == 0 ? iL_ : iR_;
            for (int i = 0; i < N; i += 4)
            {
                const float32x4_t dv = vld1q_f32(d + i);
                const int32x4_t iv = vcvtq_s32_f32(dv);
                vst1q_s32(ix + i, iv);
                vst1q_f32(fr + i, vsubq_f32(dv, vcvtq_f32_s32(iv)));
                const float32x4_t slopeG = vsubq_f32(vaddq_f32(one, vmulq_f32(dv, slope)), sub);
                vst1q_f32(g + i, vbslq_f32(vcleq_f32(dv, knee), slopeG, one));
            }
        }
        return;
    }
#endif
    for (int i = 0; i < N; ++i)
    {
        gL_[i] = delayGain(dL_[i]);
        gR_[i] = delayGain(dR_[i]);
        iL_[i] = static_cast<std::int32_t>(dL_[i]);
        iR_[i] = static_cast<std::int32_t>(dR_[i]);
        frL_[i] = dL_[i] - static_cast<float>(iL_[i]);
        frR_[i] = dR_[i] - static_cast<float>(iR_[i]);
    }
}

void Flanger::loopScalar(float* l, float* r) noexcept
{
    APP_FX_NOFUSE
    const int ln = ln_;
    float* rb0 = ring_.data();
    float* rb1 = rb0 + cap_;
    const float fb = fb_;
    float y0 = y_[0], y1 = y_[1];
    int w = w_;
    for (int i = 0; i < N; ++i)
    {
        int rL = w - iL_[i];
        if (rL < 0)
            rL += ln;
        int rR = w - iR_[i];
        if (rR < 0)
            rR += ln;
        rb0[w] = l[i] + y0 * fb;
        rb1[w] = r[i] + y1 * fb;
        if (++w >= ln)
            w = 0;
        float fr = frL_[i];
        float p = rL < 1 ? rb0[ln - 1] : rb0[rL - 1];
        y0 = (1.0f - fr) * rb0[rL] + fr * p;
        fr = frR_[i];
        p = rR < 1 ? rb1[ln - 1] : rb1[rR - 1];
        y1 = (1.0f - fr) * rb1[rR] + fr * p;
        l[i] = y0;
        r[i] = y1;
    }
    w_ = w;
    y_[0] = y0;
    y_[1] = y1;
}

void Flanger::loopNeon(float* l, float* r) noexcept
{
#if APP_FX_HAVE_NEON
    const int ln = ln_;
    float* rb0 = ring_.data();
    float* rb1 = rb0 + cap_;
    const float32x2_t fbv = vdup_n_f32(fb_), one = vdup_n_f32(1.0f);
    float32x2_t y = vld1_f32(y_);
    int w = w_;
    for (int i = 0; i < N; ++i)
    {
        int rL = w - iL_[i];
        if (rL < 0)
            rL += ln;
        int rR = w - iR_[i];
        if (rR < 0)
            rR += ln;
        float32x2_t x = vdup_n_f32(0.0f);
        x = vld1_lane_f32(l + i, x, 0);
        x = vld1_lane_f32(r + i, x, 1);
        const float32x2_t wv = vadd_f32(x, vmul_f32(y, fbv));
        vst1_lane_f32(rb0 + w, wv, 0);
        vst1_lane_f32(rb1 + w, wv, 1);
        if (++w >= ln)
            w = 0;
        const int pL = rL < 1 ? ln - 1 : rL - 1, pR = rR < 1 ? ln - 1 : rR - 1;
        float32x2_t t0 = vdup_n_f32(0.0f), t1 = vdup_n_f32(0.0f), fr = vdup_n_f32(0.0f);
        t0 = vld1_lane_f32(rb0 + rL, t0, 0);
        t0 = vld1_lane_f32(rb1 + rR, t0, 1);
        t1 = vld1_lane_f32(rb0 + pL, t1, 0);
        t1 = vld1_lane_f32(rb1 + pR, t1, 1);
        fr = vld1_lane_f32(frL_ + i, fr, 0);
        fr = vld1_lane_f32(frR_ + i, fr, 1);
        y = vadd_f32(vmul_f32(vsub_f32(one, fr), t0), vmul_f32(fr, t1));
        vst1_lane_f32(l + i, y, 0);
        vst1_lane_f32(r + i, y, 1);
    }
    vst1_f32(y_, y);
    w_ = w;
#else
    loopScalar(l, r);
#endif
}

void Flanger::gainStage(float* l, float* r) noexcept // after the loop: out *= g(d) per sample (FX-223)
{
    APP_FX_NOFUSE
#if APP_FX_HAVE_NEON
    if (! scalar_)
    {
        for (int i = 0; i < N; i += 4)
        {
            vst1q_f32(l + i, vmulq_f32(vld1q_f32(l + i), vld1q_f32(gL_ + i)));
            vst1q_f32(r + i, vmulq_f32(vld1q_f32(r + i), vld1q_f32(gR_ + i)));
        }
        return;
    }
#endif
    for (int i = 0; i < N; ++i)
    {
        l[i] = l[i] * gL_[i];
        r[i] = r[i] * gR_[i];
    }
}

void Flanger::process(float* left, float* right, const ProcessContext& ctx) noexcept // slot 25 0x115f58
{
    fresh_ = false;
    pBpm_.set(static_cast<float>(ctx.bpm)); // FX-049
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
        APP_FX_NOFUSE
        float hz;
        if (syncV_ != 0)
        {
            int i = dsp::mod::rintIdx(0, 7, rateV_);
            if (i < 0 || i > 6)
                i = 4; // the reference's assert path
            hz = dsp::mod::syncHz(kFrac[i][0], kFrac[i][1], bpmV_);
        }
        else
            hz = static_cast<float>(dsp::mod::rintIdx(1, 101, rateV_)) * kHundredth;
        setRate(hz);
        dirty_ = false;
    }
    // kernel slot 19 0xff9ac
    if (resetFlag_)
    {
        w_ = 0;
        std::fill(ring_.begin(), ring_.end(), 0.0f);
        y_[0] = y_[1] = 0.0f;
        resetFlag_ = false;
    }
    if (retrig_)
    {
        osc_.resetPhase(0.0f);
        retrig_ = false;
    }
    osc_.process(dL_, dR_, N);
    prepass();
    if (scalar_)
        loopScalar(left, right);
    else
        loopNeon(left, right);
    gainStage(left, right);
}
} // namespace engine::fx

APP_REGISTER_EFFECT((engine::fx::EffectDescriptor{
    .id = "flanger",
    .name = "FLANGER",
    .slots = engine::fx::kAllSlotKinds,
    .params = {"fx.flanger.rate", "fx.flanger.width", "fx.flanger.phase", "fx.flanger.delay", "fx.flanger.feedback",
               "fx.flanger.mix"},
    .options = engine::fx::kFlangerOptions,
    .visual = "lfoCurve",
    .shell = {.gateMode = static_cast<std::uint8_t>(engine::fx::GateMode::Open),
              .mixLaw = static_cast<std::uint8_t>(engine::fx::MixLaw::UnityDryK5),
              .wetHighPass = true,
              .skipWhenSilent = false,
              .clampOutput = false},
    .create = &engine::fx::make<engine::fx::Flanger>,
    .featureFlag = {}}))
