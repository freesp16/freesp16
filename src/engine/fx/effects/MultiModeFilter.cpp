// Multi-mode filter — see MultiModeFilter.h. Every routine names the reference routine (research
// docs/effects/multimode-filter.md "1.60 code map") and mirrors tools/fx/mmf.py line for line.
#include "engine/fx/effects/MultiModeFilter.h"

#include "engine/fx/EffectRegistryOrder.h"
#include "engine/fx/FxShell.h"
#include "engine/fx/dsp/filter_fdlibm.h"

#include <cmath>

namespace engine::fx::effects
{
namespace
{
using dsp::filt::rnd;
using dsp::nearlyEqual;

// Constructor "last" values of the eight LinearParameters (0x109e8c): floats 0…5, choices FILTERTYPE 6, FILTERDB 0.
constexpr float kCtorValues[8] = {0.5f, 0.5f, 0.5f, 0.0f, 0.0f, 0.0f, 6.0f, 0.0f};
constexpr float kMinFc = 5.0f;

int refTypeOf(int appType) noexcept // the clean enum (FX-069/070, FX-582) → FILTERTYPE
{
    return appType <= 0 ? 0 : (appType == 1 ? 1 : (appType == 2 ? 2 : 6));
}
int refDbOf(int appSlope) noexcept // 12 dB → FilterOrder2 (1), 24 dB → FilterOrder4 (2)
{
    return appSlope <= 0 ? 1 : 2;
}
} // namespace

// ------------------------------------------------------------------------------------------------ parameter laws

float MultiModeFilter::cutoffOf(float v) noexcept
{
    APP_FX_NOFUSE
    return static_cast<float>(std::pow(1000.0, static_cast<double>(v))) * 20.0f;
}
float MultiModeFilter::qOf(float v) noexcept
{
    APP_FX_NOFUSE
    return 0.2f + v * 9.8f;
}
float MultiModeFilter::gainDbOf(float v) noexcept
{
    APP_FX_NOFUSE
    return v * 45.0f - 30.0f;
}
float MultiModeFilter::gainOf(float v) noexcept
{
    APP_FX_NOFUSE
    return static_cast<float>(std::pow(10.0, static_cast<double>(gainDbOf(v) * 0.05f)));
}
float MultiModeFilter::glideOf(float v) noexcept
{
    APP_FX_NOFUSE
    const float s = (1.0f + v * 8.0f) * 64.0f;
    float d = s - 64.0f;
    if (d <= 0.0f)
        d = -d;
    return d < dsp::kEps ? 64.0f : s;
}
int MultiModeFilter::kindOf(int t, int db) noexcept
{
    if (t == 0 || t == 1 || t == 2)
        return db == 1 ? t : t + 3;
    return kKindOff;
}

// ------------------------------------------------------------------------------------------------ life cycle

void MultiModeFilter::prepare(double sampleRate, int maxBlock)
{
    (void) maxBlock; // FX-561: the engine block (64) is the only size process() handles
    fs_ = sampleRate;
    clear();
}

void MultiModeFilter::clear() noexcept
{
    APP_FX_NOFUSE
    const int ifs = static_cast<int>(fs_);
    const double fsr = static_cast<double>(ifs >= 10 ? ifs : 10);
    for (int k = 0; k < 7; ++k)
    {
        Stage& s = st_[k];
        s.kind = k;
        s.count = k < 3 ? 1 : (k < 6 ? 2 : 0);
        s.type = static_cast<dsp::filt::Kind>(k % 3);
        s.nyq = static_cast<float>(static_cast<int>(fsr / 2.0));
        s.fc = (s.nyq + 5.0f) * 0.5f;
        s.Q = 0.7071067811865476;
        s.f[0] = {};
        s.f[1] = {};
    }
    gate_.init(dsp::filt::gateSamples(fs_));
    pending_ = cur_ = 0;
    cutT_ = cutC_ = 1000.0f;
    cutMult_ = 0.0f;
    cutSteps_ = 0;
    qT_ = qC_ = 0.7f;
    // the static initialiser 0x13a28: 50^(1/100) = 10^(log10f(50) / 100)
    qUp_ = static_cast<float>(std::pow(10.0, static_cast<double>(dsp::fdlibm::log10f(50.0f) / 100.0f)));
    qDn_ = 1.0f / qUp_;
    qMult_ = qUp_;
    qSteps_ = 0;
    next_ = 0.0f;
    maxSteps_ = 0;
    flag40_ = dirty41_ = glide42_ = false;
    setGlide(64.0f);
    gain_.init(dsp::filt::gateSamples(fs_), 1.0f);
    for (int i = 0; i < 8; ++i)
    {
        p_[i].init(i >= 6, kCtorValues[i]);
        userSet_[i] = false;
    }
    pristine_ = true;
    armQuirks();
}

// The quirk-dependent part of a fresh instance (FX-581 / FX-056): Reference keeps the constructor's values as "last"
// (and "current" for what nobody set); Sixteen starts every unset parameter at the slot default and applies the first
// value of every parameter.
void MultiModeFilter::armQuirks() noexcept
{
    const bool trap = (quirks_ & kQuirkChangeTrap) != 0;
    for (int i = 0; i < 8; ++i)
    {
        auto& p = p_[i];
        if (! userSet_[i])
        {
            const float v = trap ? kCtorValues[i] : (i < 6 ? kDefaults[i] : (i == 6 ? 0.0f : 1.0f));
            p.curF = v;
            p.curI = static_cast<int>(v);
        }
        p.lastF = kCtorValues[i];
        p.lastI = static_cast<int>(kCtorValues[i]);
        p.untrapped = ! trap;
    }
}

void MultiModeFilter::setQuirkProfile(std::uint32_t mask) noexcept
{
    quirks_ = mask;
    if (pristine_)
        armQuirks();
}

void MultiModeFilter::setParam(int index, float value01) noexcept
{
    if (index < 0 || index >= 6)
        return;
    p_[index].set(value01);
    userSet_[index] = true;
}

void MultiModeFilter::setOption(int which, int value) noexcept
{
    if (which == kOptType)
    {
        p_[6].setChoice(refTypeOf(value));
        userSet_[6] = true;
    }
    else if (which == kOptSlope)
    {
        p_[7].setChoice(refDbOf(value));
        userSet_[7] = true;
    }
    else if (which == kClearState && value != 0)
        clear();
}

// slot 27 (0x107fa8)
void MultiModeFilter::reset() noexcept
{
    pristine_ = false;
    applyParams();
    Stage& s = st_[cur_];
    for (int i = 0; i < s.count; ++i)
        s.f[i].clearTaps();
}

// ------------------------------------------------------------------------------------------------ parameters

void MultiModeFilter::setGlide(float g) noexcept // container set_glide_steps
{
    APP_FX_NOFUSE
    g44_ = g;
    const float e = dsp::fdlibm::log10f(1000.0f) / g;
    up48_ = static_cast<float>(std::pow(10.0, static_cast<double>(e)));
    dn4c_ = 1.0f / up48_;
}

void MultiModeFilter::setKind(int k) noexcept // tail of 0x107c24
{
    pending_ = k;
    if (k != cur_)
        gate_.close();
}

// 0x107d18: CUTOFF, Q, GAIN, GLIDE, then FILTERTYPE + FILTERDB (both evaluated every block); index 4 is never read.
void MultiModeFilter::applyParams() noexcept
{
    if (p_[0].changed())
    {
        cutT_ = cutoffOf(p_[0].get());
        dirty41_ = true;
    }
    if (p_[2].changed())
    {
        qT_ = qOf(p_[2].get());
        dirty41_ = true;
    }
    if (p_[1].changed())
        gain_.setTarget(gainOf(p_[1].get()));
    if (p_[3].changed())
    {
        const float g = glideOf(p_[3].get());
        glide42_ = g != 64.0f;
        setGlide(g);
    }
    const bool c6 = p_[6].changed();
    const bool c7 = p_[7].changed();
    if (c6 || c7)
        setKind(kindOf(rnd(p_[6].get()), rnd(p_[7].get())));
    (void) p_[4].changed(); // NONE: registered, never read by the DSP (FX-062)
    (void) p_[5].changed(); // MIXRATIO: the shell's dry/wet (FX-524)
}

// 0x108608
void MultiModeFilter::rampSetup() noexcept
{
    APP_FX_NOFUSE
    using dsp::fdlibm::log10f;
    if (dirty41_)
    {
        const int n = rnd(dsp::absf((1.0f / log10f(up48_)) * log10f(cutT_ / cutC_)));
        cutSteps_ = n + 1;
        cutMult_ = cutT_ > cutC_ ? up48_ : dn4c_;
        const int n2 = rnd(dsp::absf((1.0f / log10f(qUp_)) * log10f(qT_ / qC_)));
        qSteps_ = n2 + 1;
        qMult_ = qT_ > qC_ ? qUp_ : qDn_;
        next_ = 44.1f;
        dirty41_ = false;
    }
    maxSteps_ = qSteps_ > cutSteps_ ? qSteps_ : cutSteps_;
    if (gate_.fadingOut())
        return;
    if (static_cast<double>(cutSteps_) > static_cast<double>(g44_) * 152.0 / 1000.0 && ! glide42_)
        gate_.close(); // FX-522 gated jump
}

// container _swap: the pending kind at its target cutoff and Q, steps cleared, re-seed armed
MultiModeFilter::Stage& MultiModeFilter::swapKind() noexcept
{
    cur_ = pending_;
    cutC_ = cutT_;
    qC_ = qT_;
    cutSteps_ = qSteps_ = maxSteps_ = 0;
    Stage& s = st_[cur_];
    configure(s, cutC_, qC_, true);
    return s;
}

// wrapper configure (kinds 0–2 inline in 0x108808, kinds 3–5 = 0x107b64: Q' = √Q), fc clamped to [5 Hz, Nyquist]
void MultiModeFilter::configure(Stage& s, float cut, float q, bool flag) noexcept
{
    if (s.count == 0)
        return;
    const double qq = s.kind < 3 ? static_cast<double>(q) : std::sqrt(static_cast<double>(q));
    const int ifs = static_cast<int>(fs_);
    const double fsr = static_cast<double>(ifs >= 10 ? ifs : 10);
    float v = s.nyq;
    if (cut <= v)
        v = cut;
    float fc = kMinFc;
    if (fc <= v)
        fc = v;
    s.Q = qq;
    s.fc = fc;
    const auto c = dsp::filt::bilinear(s.type, fc, qq, fsr);
    for (int i = 0; i < s.count; ++i)
    {
        s.f[i].c = c;
        s.f[i].reseed = flag;
    }
}

const dsp::filt::Coefs& MultiModeFilter::coefs(int stage) const noexcept
{
    return st_[cur_].f[stage & 1].c;
}

// ------------------------------------------------------------------------------------------------ processing

template <bool Neon> void MultiModeFilter::runStage(Stage& s, float* l, float* r, int n) noexcept
{
    const bool qPos = s.Q > 0.0;
    if (s.count == 1)
        dsp::filt::iirRun<Neon>(s.f[0], qPos, l, r, n);
    else if (s.count == 2)
        dsp::filt::iirRun2<Neon>(s.f[0], s.f[1], qPos, l, r, n);
}

// 0x108808
template <bool Neon> void MultiModeFilter::container(float* l, float* r, int n) noexcept
{
    APP_FX_NOFUSE
    if (cur_ != kKindOff)
        rampSetup();
    if (gate_.fadingOut())
    {
        const int rem = gate_.r.rem;
        if (n < rem)
        {
            runStage<Neon>(st_[cur_], l, r, n);
            gate_.apply<Neon>(l, r, n);
            return;
        }
        runStage<Neon>(st_[cur_], l, r, rem);
        gate_.apply<Neon>(l, r, rem);
        Stage& s = swapKind();
        runStage<Neon>(s, l + rem, r + rem, n - rem);
        gate_.open();
        gate_.apply<Neon>(l + rem, r + rem, n - rem);
        return;
    }
    if (gate_.closedSettled()) // only a freshly built filter: switch now, then fade in (FX-580)
    {
        Stage& s = swapKind();
        runStage<Neon>(s, l, r, n);
        if ((quirks_ & kQuirkStartupGate) != 0)
            gate_.open();
        else
            gate_.setNow(true); // Sixteen: the gate is open from the first sample
        gate_.apply<Neon>(l, r, n);
        return;
    }
    int rem = n, pos = 0;
    while (rem > 0) // open or fading in: the stepped cutoff / Q glide (FX-521)
    {
        int chunk;
        if (maxSteps_ <= 0)
        {
            chunk = rem;
            rem = 0;
        }
        else
        {
            const float s15 = next_;
            const int r6 = rnd(s15);
            if (rem >= r6)
            {
                const float s14 = static_cast<float>(r6);
                rem -= r6;
                const int ocs = cutSteps_;
                cutSteps_ = ocs > 1 ? ocs - 1 : 0;
                const int oqs = qSteps_;
                next_ = (s15 - s14) + 44.1f;
                cutC_ = ocs > 1 ? cutC_ * cutMult_ : cutT_;
                qSteps_ = oqs > 1 ? oqs - 1 : 0;
                flag40_ = true;
                qC_ = oqs > 1 ? qC_ * qMult_ : qT_;
                chunk = r6;
            }
            else
            {
                chunk = rem;
                next_ = s15 - static_cast<float>(rem);
                rem = 0;
            }
        }
        Stage& s = st_[cur_];
        runStage<Neon>(s, l + pos, r + pos, chunk);
        pos += chunk;
        if (flag40_)
        {
            configure(s, cutC_, qC_, false);
            flag40_ = false;
        }
    }
    gate_.apply<Neon>(l, r, n);
}

// slot 25 (0x108ee4): parameters → filter container → output gain ramp (FX-523: the filter-then-gain order)
template <bool Neon> void MultiModeFilter::run(float* l, float* r) noexcept
{
    pristine_ = false;
    applyParams();
    container<Neon>(l, r, kBlockFrames);
    gain_.process<Neon>(l, r, kBlockFrames);
}

void MultiModeFilter::process(float* left, float* right, const ProcessContext&) noexcept
{
    run<APP_FX_HAVE_NEON != 0>(left, right);
}

void MultiModeFilter::processScalar(float* left, float* right, const ProcessContext&) noexcept
{
    run<false>(left, right);
}
} // namespace engine::fx::effects

namespace
{
constexpr std::string_view kFilterOptions[] = {"fx.filter.type1", "fx.filter.type2"};
}

APP_REGISTER_EFFECT((engine::fx::EffectDescriptor{
    .id = "filter",
    .name = "MULTI MODE FILTER",
    .slots = engine::fx::kAllSlotKinds,
    .params = {"fx.filter.cutoff", "fx.filter.gain", "fx.filter.q", "fx.filter.glide", "", "fx.filter.mix"},
    .options = kFilterOptions,
    .visual = "filterCurve",
    // FX-524: crossfade (dry 1 − v, wet v), then the 10 Hz wet high-pass; gate open, no skip/clamp
    .shell = {.gateMode = static_cast<std::uint8_t>(engine::fx::GateMode::Open),
              .mixLaw = static_cast<std::uint8_t>(engine::fx::MixLaw::CrossfadeK5),
              .wetHighPass = true,
              .skipWhenSilent = false,
              .clampOutput = false},
    .create = &engine::fx::make<engine::fx::effects::MultiModeFilter>,
    .featureFlag = {}}))
