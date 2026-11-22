// FxShell — see FxShell.h. The comments name the reference slots of research docs/effects/effectbase.md and the
// functions of the MIT model tools/fx/effectbase.py that each block mirrors.
#include "engine/fx/FxShell.h"

#include "engine/fx/dsp/Vec.h"

namespace engine::fx
{
namespace mix
{
void wetOnly(const float*, float& dry, float& wet) noexcept
{
    dry = 0.0f;
    wet = 1.0f;
}
void unityDryWetK5(const float* p, float& dry, float& wet) noexcept
{
    dry = 1.0f;
    wet = p[5];
}
void crossfadeK5(const float* p, float& dry, float& wet) noexcept
{
    dry = 1.0f - p[5];
    wet = p[5];
}
void delayPiecewise(const float* p, float& dry, float& wet) noexcept // insert_delay.py / send_delay.py update_mix
{
    APP_FX_NOFUSE
    const float v = p[5];
    if (v < 0.75f)
    {
        dry = 1.0f;
        wet = v * 1.3333334f;
    }
    else // also NaN (the reference's `v >= 0.75 or isnan(v)`)
    {
        wet = 1.0f;
        const float d = (v - 0.75f) * -4.0f;
        dry = 1.0f + d;
    }
}
void reverbPiecewise(const float* p, float& dry, float& wet) noexcept // send_reverb.py: wet × f32(10^(−3/20))
{
    delayPiecewise(p, dry, wet);
    wet = wet * 0.70794576f;
}
} // namespace mix

MixLawFn mixLawFor(std::uint8_t law) noexcept
{
    switch (static_cast<MixLaw>(law))
    {
    case MixLaw::WetOnly:
        return &mix::wetOnly;
    case MixLaw::UnityDryK5:
        return &mix::unityDryWetK5;
    case MixLaw::CrossfadeK5:
        return &mix::crossfadeK5;
    case MixLaw::DelayPiecewise:
        return &mix::delayPiecewise;
    case MixLaw::ReverbPiecewise:
        return &mix::reverbPiecewise;
    case MixLaw::EffectDefined:
    case MixLaw::Count:
        break;
    }
    return nullptr;
}

namespace
{
constexpr float kFadeMs = 3.3333333f; // K_FADE_MS

void fillConst(float* g, float v, int n) noexcept
{
    for (int i = 0; i < n; ++i)
        g[i] = v;
}
} // namespace

// -------------------------------------------------------------------------------------------- Gate (InOutGain)

void FxShell::Gate::prepare(double fs, bool inputFade) noexcept
{
    for (auto* s : {&in, &dry, &wet})
    {
        s->prepare(fs);
        s->setTime(kFadeMs);
    }
    cur = trans = tgt = pend = pstate = 0;
    prec = Record{0, 0.0f, 1, 1.0f, 1, 0.0f};
    c0 = 1.0f;
    c4 = 0.0f;
    mode1 = inputFade;
}

void FxShell::Gate::start(int state, const Record& r) noexcept // 0x107310
{
    pstate = state;
    pend = 1;
    prec = r;
}

void FxShell::Gate::reset(const Record& r) noexcept // 0x1072bc
{
    cur = trans = tgt = 0;
    in.snap(r.in);
    dry.snap(r.dry);
    wet.snap(r.wet);
}

void FxShell::Gate::applyPending() noexcept // 0x107374
{
    if (! pend)
        return;
    const int next = pstate, old = tgt;
    pend = 0;
    if (next == old)
        return;
    if (trans) // FX-026: a new target during a transition freezes every ramp where it is
    {
        cur = old;
        in.snap(in.current());
        dry.snap(dry.current());
        wet.snap(wet.current());
    }
    trans = 1;
    tgt = next;
    in.setTime(kFadeMs);
    dry.setTime(kFadeMs);
    wet.setTime(kFadeMs);
    const std::int8_t modes[3] = {prec.inMode, prec.dryMode, prec.wetMode};
    const float values[3] = {prec.in, prec.dry, prec.wet};
    dsp::SmoothFloat* s[3] = {&in, &dry, &wet};
    for (int i = 0; i < 3; ++i)
    {
        if (modes[i] == 0)
            s[i]->snap(values[i]);
        else if (modes[i] == 1)
            s[i]->setTarget(values[i]);
    }
}

int FxShell::Gate::running() const noexcept // 0x1079d0 (slot 9)
{
    const int c = cur, t = tgt;
    if (c != t)
    {
        if (c == 0)
            return t;
        if (t != 0)
        {
            if (c == 2)
                return t == 1 ? 1 : 0;
            if (c != 1)
                return 0;
            return t == 2 ? 1 : 0;
        }
    }
    return c;
}

void FxShell::Gate::mix(float* l, float* r, float* cl, float* cr, int n) noexcept // 0x10770c
{
    if (! trans)
    {
        if (cur == 1)
        {
            dry.setTarget(c0);
            wet.setTarget(c4);
        }
        if (cur == 2 && mode1)
            wet.setTarget(c4);
    }
    if (! dry.ramping() && ! wet.ramping())
    {
        const float d = dry.steady(), w = wet.steady();
        if (w == 0.0f) // the reference zero-fills the copy: a NaN/Inf wet never reaches the output
        {
            if (d == 0.0f)
            {
                std::memset(l, 0, sizeof(float) * static_cast<std::size_t>(n));
                std::memset(r, 0, sizeof(float) * static_cast<std::size_t>(n));
            }
            else if (d != 1.0f)
            {
                dsp::vec::mul(l, d, n);
                dsp::vec::mul(r, d, n);
            }
        }
        else if (d == 0.0f) // dry zero-filled: out = copy·wet
        {
            if (w != 1.0f)
            {
                dsp::vec::mul(cl, w, n);
                dsp::vec::mul(cr, w, n);
            }
            dsp::vec::copy(l, cl, n);
            dsp::vec::copy(r, cr, n);
        }
        else
        {
            dsp::vec::mix(l, cl, d, w, n);
            dsp::vec::mix(r, cr, d, w, n);
        }
    }
    else
    {
        alignas(16) float gd[dsp::kMaxBlock];
        alignas(16) float gw[dsp::kMaxBlock];
        if (dry.ramping())
            dry.block(gd, n);
        else
            fillConst(gd, dry.steady(), n);
        if (wet.ramping())
            wet.block(gw, n);
        else
            fillConst(gw, wet.steady(), n);
        dsp::vec::mixv(l, cl, gd, gw, n);
        dsp::vec::mixv(r, cr, gd, gw, n);
    }
    if (trans && ! in.ramping() && ! dry.ramping() && ! wet.ramping())
    {
        cur = tgt;
        trans = 0;
        in.setTime(kFadeMs);
        dry.setTime(kFadeMs);
        wet.setTime(kFadeMs);
    }
}

// -------------------------------------------------------------------------------------------- FxShell

void FxShell::prepare(EffectBase& effect, const ShellPolicy& policy, double sampleRate, SlotKind kind, float* scratchL,
                      float* scratchR) noexcept
{
    effect_ = &effect;
    policy_ = policy;
    law_ = mixLawFor(policy.mixLaw);
    kind_ = kind;
    fs_ = sampleRate;
    fade_ = dsp::shellFadeSamples(sampleRate);
    cl_ = scratchL;
    cr_ = scratchR;
    for (auto& p : params_)
        p = 0.0f;
    optionSet_ = 0;
    clearPending_ = everActivated_ = false;
    dry_ = 1.0f;
    wet_ = 0.0f;
    skipEn_ = clampEn_ = skipArmed_ = clampArmed_ = false;
    skip_ = clamp_ = 0;
    gate_.prepare(sampleRate, policy.gateMode == static_cast<std::uint8_t>(GateMode::InputFade));
    hpfReq_ = policy.wetHighPass;
    hpfCur_ = false;
    hpfFc_ = 10.0f;
    hpf_.prepare(sampleRate);
    gateA_.prepare(sampleRate, fade_);
    gateB_.prepare(sampleRate, fade_);
    // The constructor's records: the mode-0 pattern with dry 1 / wet 0, R0 in ramp mode.
    const std::uint8_t mode = policy_.gateMode;
    policy_.gateMode = 0;
    buildRecords();
    policy_.gateMode = mode;
    recs_[0].inMode = recs_[0].dryMode = recs_[0].wetMode = 1;
    updateMix();
}

void FxShell::buildRecords() noexcept // slot 31 0x105d78 (effectbase.py _records)
{
    const float one = 1.0f, zero = 0.0f;
    switch (policy_.gateMode)
    {
    case 0:
    case 2:
    {
        const std::int8_t r1in = policy_.gateMode == 0 ? 1 : 0;
        recs_[0] = {-1, one, -1, one, -1, zero};
        recs_[1] = {r1in, one, 0, one, 0, zero};
        recs_[2] = {0, one, 1, one, 1, zero};
        recs_[3] = {0, one, 1, dry_, 1, wet_};
        recs_[4] = {0, one, 1, one, 1, zero};
        break;
    }
    case 1:
        recs_[0] = {-1, zero, -1, one, -1, zero};
        recs_[1] = {0, zero, 0, one, 1, wet_};
        recs_[2] = {1, zero, 1, one, 1, zero};
        recs_[3] = {1, one, 1, dry_, 1, wet_};
        recs_[4] = {1, zero, 1, one, 0, wet_};
        break;
    default:
        break; // other modes keep the records (reference)
    }
}

void FxShell::updateMix() noexcept // slot 26
{
    if (law_ != nullptr)
        law_(params_, dry_, wet_);
    else if (policy_.mixLaw == static_cast<std::uint8_t>(MixLaw::EffectDefined) && effect_ != nullptr)
    {
        dry_ = effect_->telemetry(kTelemetryDry);
        wet_ = effect_->telemetry(kTelemetryWet);
    }
}

void FxShell::sendState() noexcept // FX-056: the full current parameter set on every activation
{
    for (int k = 0; k < 6; ++k)
        effect_->setParam(k, params_[k]);
    for (int i = 0; i < kMaxOptions; ++i)
        if ((optionSet_ >> i) & 1u)
            effect_->setOption(i, options_[i]);
}

void FxShell::activate(bool honourSkip) noexcept
{
    // Sixteen: quirk profile (FX-053), pending FX-036 clear and the full parameter set (FX-056) before the reset.
    effect_->setQuirkProfile(pendingQuirks_);
    const bool quirk = kind_ == SlotKind::Insert && (pendingQuirks_ & kQuirkShellSkipClamp) != 0;
    skipEn_ = quirk && policy_.skipWhenSilent;
    clampEn_ = quirk && policy_.clampOutput;
    if (clearPending_)
    {
        effect_->setOption(kClearState, 1);
        clearPending_ = false;
    }
    sendState();
    everActivated_ = true;

    // slot 5 (0x10623c), a = 0.0
    if (honourSkip && skipEn_)
    {
        clampArmed_ = false;
        skipArmed_ = true;
        clamp_ = 0;
        skip_ = fade_;
    }
    hpfCur_ = hpfReq_;
    hpf_.zero();
    hpf_.setCutoff(hpfFc_);
    gateA_.setNow(hpfCur_);
    gateB_.setNow(hpfCur_);
    effect_->reset(); // slot 27
    updateMix();      // slot 26
    buildRecords();
    recs_[1].dry = 0.0f;
    gate_.reset(recs_[0]);
    gate_.start(2, recs_[1]);
    // slot 7 (0x105c58): the processors' start variant (+0x9d = 1)
    buildRecords();
    gate_.start(1, recs_[3]);
}

void FxShell::stop() noexcept // slot 6 (0x105c94), a = 1.0
{
    if (clampEn_)
    {
        skipArmed_ = false;
        clampArmed_ = true;
        skip_ = 0;
        clamp_ = fade_;
    }
    buildRecords();
    recs_[2].dry = 1.0f;
    gate_.start(0, recs_[2]);
}

void FxShell::stopWet() noexcept // slot 8 (0x105c20)
{
    buildRecords();
    gate_.start(2, recs_[4]);
}

bool FxShell::running() const noexcept
{
    return gate_.running() != 0;
}

void FxShell::setParam(int k, float v) noexcept
{
    if (k < 0 || k >= 6)
        return;
    v = v >= 0.0f ? (v <= 1.0f ? v : 1.0f) : 0.0f; // FX-057; NaN → 0
    params_[k] = v;
    if (effect_ != nullptr)
        effect_->setParam(k, v);
    updateMix();
}

void FxShell::setOption(int which, int value) noexcept
{
    if (which >= 0 && which < kMaxOptions)
    {
        options_[which] = value;
        optionSet_ |= 1u << which;
    }
    if (effect_ != nullptr)
        effect_->setOption(which, value);
}

void FxShell::clearNow() noexcept
{
    effect_->setOption(kClearState, 1);
    clearPending_ = false;
    sendState();
    effect_->reset();
    hpf_.zero();
    updateMix();
}

void FxShell::post(float* cl, float* cr, int n) noexcept // 0x106c88
{
    if (hpfCur_ != hpfReq_)
    {
        hpfCur_ = hpfReq_;
        if (! hpfCur_)
        {
            gateA_.close();
            gateB_.close();
        }
        else
        {
            hpf_.setCutoff(hpfFc_);
            gateA_.open();
            gateB_.open();
        }
    }
    if (! hpfCur_ && gateA_.settledClosed() && gateB_.settledClosed())
        return;
    gateA_.apply(cl, cr, n);
    hpf_.process(cl, cr, n);
    gateB_.apply(cl, cr, n);
}

bool FxShell::process(float* left, float* right, const ProcessContext& ctx) noexcept // slot 18 (0x106de4)
{
    gate_.applyPending();
    if (! gate_.running())
        return false;
    int i0 = 0, n = kBlockFrames;
    if (skipArmed_)
    {
        const int c = skip_;
        if (n < c)
        {
            skip_ = c - n;
            return false;
        }
        n -= c;
        i0 = c;
        skipArmed_ = false;
        skip_ = 0;
    }
    if (clampArmed_)
    {
        const int c = clamp_;
        if (n < c)
            clamp_ = c - n;
        else
        {
            clamp_ = 0;
            clampArmed_ = false;
            n = c;
        }
    }
    dsp::vec::copy(cl_, left, kBlockFrames);
    dsp::vec::copy(cr_, right, kBlockFrames);
    gate_.in.apply(cl_ + i0, cr_ + i0, n); // input fade 0x1074c4 (a no-op at gain 1 in mode 0)
    effect_->process(cl_, cr_, ctx);       // slot 25: 64 frames, wet only
    if (law_ == nullptr)
        updateMix(); // EffectDefined: the reference copies the effect's dry/wet every block
    post(cl_ + i0, cr_ + i0, n);
    gate_.c0 = dry_;
    gate_.c4 = wet_;
    gate_.mix(left + i0, right + i0, cl_ + i0, cr_ + i0, n);
    return true;
}
} // namespace engine::fx
