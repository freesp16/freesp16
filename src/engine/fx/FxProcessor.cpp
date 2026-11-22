// FxProcessor — see FxProcessor.h. Comments name the functions of the MIT model tools/fx/fx_processors.py mirrored.
#include "engine/fx/FxProcessor.h"

#include "engine/fx/dsp/Vec.h"

namespace engine::fx
{
void FxProcessor::build(SlotKind kind, std::span<const EffectDescriptor* const> types)
{
    kind_ = kind;
    types_.assign(types.begin(), types.end());
    effects_.clear();
    effects_.reserve(types_.size()); // rt-ok: message thread, build()
    for (const auto* d : types_)
        effects_.push_back(d->create()); // rt-ok: message thread, one instance per type (FX-005)
    shells_.clear();
    shells_.resize(types_.size()); // rt-ok: message thread, build()
    cur_ = pend_ = -1;
}

bool FxProcessor::prepare(double sampleRate, int maxBlock)
{
    if (maxBlock < kBlockFrames)
        return false;
    fs_ = sampleRate;
    for (std::size_t i = 0; i < effects_.size(); ++i)
    {
        if (! effects_[i])
            return false;
        effects_[i]->prepare(sampleRate, maxBlock);
        shells_[i].prepare(*effects_[i], types_[i]->shell, sampleRate, kind_, copyL_, copyR_);
    }
    const int nMute = dsp::muteFadeSamples(sampleRate);
    // Send: Sixteen starts muted when Off (FX-048's start-up ramp is a reference quirk); the reference processor starts
    // with the mute open (fx_processors.py SendProcessor: MuteSmoother(n, 0)) — see setReferenceStartUp.
    ms_.prepare(nMute, kind_ == SlotKind::Send);
    kill_.prepare(nMute, false);
    byp_.prepare(nMute, false);
    killPhase_ = 0;
    cur_ = pend_ = -1;
    bypass_ = disabled_ = false;
    inPeak_ = outPeak_ = 0.0f;
    updateIdle();
    return true;
}

void FxProcessor::setReferenceStartUp() noexcept
{
    ms_.prepare(dsp::muteFadeSamples(fs_), false);
    idle_ = false;
}

const EffectDescriptor* FxProcessor::descriptor(int type) const noexcept
{
    return valid(type) ? types_[static_cast<std::size_t>(type)] : nullptr;
}

EffectBase* FxProcessor::effect(int type) const noexcept
{
    return valid(type) ? effects_[static_cast<std::size_t>(type)].get() : nullptr;
}

FxShell* FxProcessor::shell(int type) noexcept
{
    return valid(type) ? &shells_[static_cast<std::size_t>(type)] : nullptr;
}

const FxShell* FxProcessor::shell(int type) const noexcept
{
    return valid(type) ? &shells_[static_cast<std::size_t>(type)] : nullptr;
}

int FxProcessor::latencySamples() const noexcept
{
    return valid(cur_) ? effects_[static_cast<std::size_t>(cur_)]->latencySamples() : 0;
}

float FxProcessor::telemetry(int which) const noexcept
{
    return valid(cur_) ? effects_[static_cast<std::size_t>(cur_)]->telemetry(which) : 0.0f;
}

double FxProcessor::clampTempo(double bpm) noexcept // fx_processors.py clamp_tempo
{
    const float f = static_cast<float>(bpm);
    if (f < 1.0f)
        return 1.0;
    if (f > 999.0f)
        return 999.0;
    return static_cast<double>(f);
}

void FxProcessor::requestType(int n) noexcept // set_fx_type (Master 0x1132e4 = Send 0x11a254 = Insert 0x11735c)
{
    if (! valid(n))
        n = -1;
    if (pend_ == n)
        return;
    idle_ = false;
    if (cur_ == -1)
    {
        cur_ = pend_ = n;
        return;
    }
    shells_[static_cast<std::size_t>(cur_)].stop();
    pend_ = n;
}

void FxProcessor::setBypass(bool on) noexcept
{
    if (bypass_ == on)
        return;
    bypass_ = on;
    idle_ = false;
}

void FxProcessor::setDisabled(bool on) noexcept // FxProcessorBase 0x112a08
{
    if (disabled_ == on)
        return;
    disabled_ = on;
    idle_ = false;
    if (on && valid(cur_))
        shells_[static_cast<std::size_t>(cur_)].stop();
}

void FxProcessor::setParam(int type, int k, float v01) noexcept
{
    if (valid(type))
        shells_[static_cast<std::size_t>(type)].setParam(k, v01);
}

void FxProcessor::setOption(int type, int which, int value) noexcept
{
    if (valid(type))
        shells_[static_cast<std::size_t>(type)].setOption(which, value);
}

void FxProcessor::setQuirkProfile(std::uint32_t mask) noexcept
{
    for (auto& s : shells_)
        s.setQuirkProfile(mask);
}

void FxProcessor::retrigger() noexcept
{
    if (valid(cur_))
        effects_[static_cast<std::size_t>(cur_)]->retrigger();
    if (valid(pend_) && pend_ != cur_)
        effects_[static_cast<std::size_t>(pend_)]->retrigger();
}

void FxProcessor::silenceAndClear() noexcept
{
    if (cur_ < 0 && pend_ < 0) // nothing sounds: mark every instance, no fade needed
    {
        for (auto& s : shells_)
            s.clearOnNextActivation();
        return;
    }
    kill_.setMuted(true);
    killPhase_ = 1;
    idle_ = false;
}

void FxProcessor::swap() noexcept // _FxProcessor._swap
{
    const int p = pend_;
    cur_ = p;
    if (p >= 0 && ! disabled_)
        shells_[static_cast<std::size_t>(p)].activate(kind_ != SlotKind::Send);
}

void FxProcessor::run(float* l, float* r, const ProcessContext& ctx) noexcept // _FxProcessor._run
{
    if (cur_ < 0)
        return;
    shells_[static_cast<std::size_t>(cur_)].process(l, r, ctx);
}

void FxProcessor::killStep() noexcept
{
    if (killPhase_ == 1 && kill_.state() == dsp::MuteSmoother::Closed)
    {
        for (std::size_t t = 0; t < shells_.size(); ++t)
        {
            auto& s = shells_[t];
            if (static_cast<int>(t) == cur_ && s.running())
                s.clearNow();
            else
                s.clearOnNextActivation();
        }
        kill_.setMuted(false);
        killPhase_ = 2;
    }
    else if (killPhase_ == 2 && kill_.state() == dsp::MuteSmoother::Open)
        killPhase_ = 0;
}

void FxProcessor::updateIdle() noexcept
{
    if (killPhase_ != 0)
    {
        idle_ = false;
        return;
    }
    const bool settled =
        cur_ < 0 || (pend_ == cur_ && ! shells_[static_cast<std::size_t>(cur_)].running() && disabled_);
    switch (kind_)
    {
    case SlotKind::Insert:
        idle_ = settled && ms_.state() == dsp::MuteSmoother::Open;
        break;
    case SlotKind::Master: // settled = the input passes, bypassed or not: only a bypass ramp keeps it busy
        idle_ = settled && byp_.state() != dsp::MuteSmoother::Ramp;
        break;
    case SlotKind::Send:
        // Muted input and nothing left to swap or to ring: Off after its swap, or disabled with the stop done.
        idle_ = ms_.state() == dsp::MuteSmoother::Closed && byp_.state() != dsp::MuteSmoother::Ramp &&
                (cur_ < 0 ? pend_ == -1 : disabled_ && ! shells_[static_cast<std::size_t>(cur_)].running());
        break;
    }
}

bool FxProcessor::process(float* l, float* r, const ProcessContext& ctx) noexcept
{
    if (idle_)
        return kind_ != SlotKind::Send;
    if (metered_)
    {
        const float a = dsp::vec::peakAbs(l, kBlockFrames), b = dsp::vec::peakAbs(r, kBlockFrames);
        inPeak_ = a > b ? a : b;
    }
    ProcessContext c = ctx;
    c.bpm = clampTempo(ctx.bpm);   // FX-049
    if (kind_ != SlotKind::Insert) // FX-050: no triggers on the bus slots
    {
        c.triggerMask = 0;
        c.triggerOffset.fill(-1);
    }
    switch (kind_)
    {
    case SlotKind::Insert: // InsertProcessorFull.process_block
    {
        ms_.setMuted(bypass_);
        const bool crossfade = ms_.state() != dsp::MuteSmoother::Open;
        if (crossfade)
        {
            dsp::vec::copy(dryL_, l, kBlockFrames);
            dsp::vec::copy(dryR_, r, kBlockFrames);
        }
        if (cur_ >= 0 && ! shells_[static_cast<std::size_t>(cur_)].running())
            swap();
        run(l, r, c);
        if (crossfade)
            ms_.crossfade(l, r, dryL_, dryR_, kBlockFrames);
        break;
    }
    case SlotKind::Master: // MasterProcessor.process_block (+ the FX-046 bypass: the main bus dry)
    {
        byp_.setMuted(bypass_);
        const bool crossfade = byp_.state() != dsp::MuteSmoother::Open;
        if (crossfade)
        {
            dsp::vec::copy(dryL_, l, kBlockFrames);
            dsp::vec::copy(dryR_, r, kBlockFrames);
        }
        if (cur_ >= 0 && ! shells_[static_cast<std::size_t>(cur_)].running())
            swap();
        run(l, r, c);
        if (crossfade)
            byp_.crossfade(l, r, dryL_, dryR_, kBlockFrames);
        break;
    }
    case SlotKind::Send: // SendProcessor.process_block (+ the FX-046 bypass: the muted-input send bus to the return)
    {
        ms_.setMuted(pend_ == -1 || disabled_);
        ms_.process(l, r, kBlockFrames);
        byp_.setMuted(bypass_);
        const bool crossfade = byp_.state() != dsp::MuteSmoother::Open;
        if (crossfade)
        {
            dsp::vec::copy(dryL_, l, kBlockFrames);
            dsp::vec::copy(dryR_, r, kBlockFrames);
        }
        if (cur_ >= 0 && ! disabled_ && ! shells_[static_cast<std::size_t>(cur_)].running())
            swap();
        run(l, r, c);
        if (crossfade)
            byp_.crossfade(l, r, dryL_, dryR_, kBlockFrames);
        break;
    }
    }
    if (killPhase_ != 0)
    {
        kill_.process(l, r, kBlockFrames);
        killStep();
    }
    if (metered_)
    {
        const float a = dsp::vec::peakAbs(l, kBlockFrames), b = dsp::vec::peakAbs(r, kBlockFrames);
        outPeak_ = a > b ? a : b;
    }
    updateIdle();
    return true;
}
} // namespace engine::fx
