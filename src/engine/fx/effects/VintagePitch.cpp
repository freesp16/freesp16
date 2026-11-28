// Vintage Pitch Shifter — see VintagePitch.h. The per-sample chain is written once on dsp::vpitch::V2<Neon>, so the
// scalar reference and the NEON lanes run the same float operations in the same order; the event logic (clocks,
// pointers, crossfade) is shared scalar code. Control-rate maths (tan, exp, sin, exp2) runs once per block at most,
// and only for what changed.
#include "engine/fx/effects/VintagePitch.h"

#include "engine/fx/EffectRegistryOrder.h"
#include "engine/fx/FxShell.h"
#include "engine/fx/dsp/delay_kernels.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace engine::fx
{
namespace
{
using namespace dsp::vpitch;
using dsp::delay::rintIdx;

constexpr std::uint32_t kSeedDacA = 0xA7530A01u, kSeedDacB = 0xA7530B02u;

// Bit-weight errors of an R-2R multiplying D/A, as a per-code table at unit error: lut(c) = c/512 + k·err(c), k in
// LSBs of the MSB's relative error (equal relative error per bit, so the absolute error peaks at the MSB and the
// worst step is the 511 ↔ 512 carry: the audio zero crossing).
void buildDacError(float* err, std::uint32_t seed) noexcept
{
    Rng rng;
    rng.seed(seed);
    double w[10];
    for (int b = 0; b < 10; ++b)
        w[b] = static_cast<double>(1 << b) * static_cast<double>(rng.bipolar()) / (512.0 * 512.0);
    for (int c = 0; c < 1024; ++c)
    {
        double e = 0.0;
        for (int b = 0; b < 10; ++b)
            if ((c & (1 << b)) != 0)
                e += w[b];
        err[c] = static_cast<float>(e);
    }
}
} // namespace

// ------------------------------------------------------------------------------------------------ parameter laws

int VintagePitch::semitones(float v) noexcept
{
    return rintIdx(-12, 13, v);
}
int VintagePitch::cents(float v) noexcept
{
    return rintIdx(-50, 51, v);
}
int VintagePitch::delayMs(float v) noexcept
{
    return rintIdx(0, 121, v);
}
int VintagePitch::feedbackPct(float v) noexcept
{
    return rintIdx(-100, 101, v);
}
int VintagePitch::agePct(float v) noexcept
{
    return rintIdx(0, 101, v);
}
float VintagePitch::ratioOf(int st, int ct) noexcept
{
    const double r = std::exp2(static_cast<double>(100 * st + ct) / 1200.0);
    return static_cast<float>(std::clamp(r, static_cast<double>(kRatioMin), static_cast<double>(kRatioMax)));
}

// ------------------------------------------------------------------------------------------------ lifecycle

template <bool Neon> void VintagePitch::Chain<Neon>::reset(const dsp::vpitch::CompanderCoefs& k) noexcept
{
    comp.reset();
    aa1.reset();
    aa2.reset();
    recon.reset();
    expand.reset(k);
    out.reset();
    blep.reset();
    h0 = h1 = h2 = h3 = held = V2<Neon>::dup(0.0f);
    noise.seed(0xADC0FFEEu, 0xADC1FFEEu);
}

void VintagePitch::prepare(double sampleRate, int)
{
    fs_ = sampleRate > 8000.0 ? sampleRate : 44100.0;
    const double fs = fs_;
    aa1_.setup(fs, static_cast<double>(kAaEdgeHz) * 0.5970, 0.7051);
    aa2_.setup(fs, static_cast<double>(kAaEdgeHz) * 1.0313, 2.9406);
    recon_.setup(fs, kReconHz, kReconQ);
    comp_.att = static_cast<float>(1.0 - std::exp(-1000.0 / (static_cast<double>(kAttackMs) * fs)));
    comp_.rel = static_cast<float>(1.0 - std::exp(-1000.0 / (static_cast<double>(kReleaseMs) * fs)));
    const double pole = static_cast<double>(kEmphasisZeroHz) * std::pow(10.0, static_cast<double>(kEmphasisDb) / 20.0);
    comp_.emph.setup(fs, kEmphasisZeroHz, pole);
    comp_.deemph.setup(fs, pole, kEmphasisZeroHz);
    comp_.expCouplingR = static_cast<float>(1.0 - 6.283185307179586 * static_cast<double>(kExpCouplingHz) / fs);
    outCouplingR_ = static_cast<float>(1.0 - 6.283185307179586 * static_cast<double>(kOutCouplingHz) / fs);
    outMaxStep_ = static_cast<float>(static_cast<double>(kOutSlewFsPerUs) * 1.0e6 / fs);
    finGrid_ = static_cast<float>(static_cast<double>(kFinGridHz) / fs);
    invFinGrid_ = 1.0f / finGrid_;
    const double dt = static_cast<double>(kBlockFrames) / fs; // control period
    const double a = std::exp(-dt / 12.0);                    // VCO drift: AR(1), τ 12 s, σ 0.1 %
    driftA_ = static_cast<float>(a);
    driftB_ = static_cast<float>(0.001 * std::sqrt(1.0 - a * a));
    fmInc_ = static_cast<float>(6.283185307179586 * 5.1 * dt); // supply-ripple FM at 5.1 Hz
    limitDecay_ = static_cast<float>(std::exp(-dt / 0.3));
    buildDacError(errA_, kSeedDacA);
    buildDacError(errB_, kSeedDacB);
    init();
}

void VintagePitch::init() noexcept
{
    for (float& a : applied_)
        a = -1.0f; // every knob re-applies
    age_ = -1;
    drift_ = fmPhase_ = 0.0f;
    ctlRng_.seed(0x4046u);
    jitRng_.seed(0x5EEDF1A5u);
    limitHold_ = 0.0f;
    splices_ = limitHits_ = 0;
    applyParams();
    clearMemory();
    fbCur_ = fbTarget_;
}

void VintagePitch::clearMemory() noexcept
{
    for (auto& word : ram_)
        word[0] = word[1] = static_cast<std::int16_t>(kAdcMid);
    for (auto& word : echo_)
        word[0] = word[1] = static_cast<std::int16_t>(kAdcMid);
    w_ = 0;
    e_ = 0;
    preset_ = presetTarget_;
    readPos_ = w_ - static_cast<std::uint32_t>(preset_ + kWindow / 2); // half a window behind: room both ways
    phRef_ = phIn_ = 0.0f;
    xfCountdown_ = xfCount_ = -1;
    refA_ = 0.0f;
    scalar_.reset(comp_);
#if APP_FX_HAVE_NEON
    neon_.reset(comp_);
#endif
    std::fill(std::begin(fbL_), std::end(fbL_), 0.0f);
    std::fill(std::begin(fbR_), std::end(fbR_), 0.0f);
}

void VintagePitch::reset() noexcept // FX-032: activation / type change — an empty unit, the knobs as stored
{
    applyParams();
    clearMemory();
    fbCur_ = fbTarget_;
    limitHold_ = 0.0f;
}

void VintagePitch::setParam(int index, float value01) noexcept
{
    if (index >= 0 && index < 6)
        v_[index] = value01;
}

void VintagePitch::setOption(int which, int value) noexcept
{
    if (which == kClearState && value != 0)
        init();
}

float VintagePitch::telemetry(int which) const noexcept
{
    if (which == kTelPointer)
        return std::clamp(static_cast<float>(pointerDelay()) * (1.0f / kWindow), 0.0f, 1.0f);
    if (which == kTelLimit)
        return limitHold_;
    return 0.0f;
}

// ------------------------------------------------------------------------------------------------ control rate

void VintagePitch::setAge(int pct) noexcept
{
    age_ = pct;
    const float a = static_cast<float>(pct) * 0.01f;
    const float nl = 0.1f + 0.9f * a;   // converter quality
    bow_ = 0.002f + 0.020f * nl;        // A/D current-sink bow (2nd order)
    acq_ = 1.0f - (0.01f + 0.05f * nl); // S&H acquisition per sample
    noiseLsb_ = 0.5f + 0.5f * nl;       // async count gating
    clockDetune_ = -0.02f * a;          // the LC master clock runs slow: gain and asymmetric clip
    adcGain_ = 1.0f + clockDetune_;
    adcOffset_ = 0.5f * clockDetune_;
    deadLsbs_ = pct < 60 ? 0 : (pct < 85 ? 1 : (pct < 97 ? 2 : 3));
    codeMask_ = ~((1 << deadLsbs_) - 1);
    inst_ = 0.15f + 0.85f * a;       // VCO drift, FM and period jitter
    comp_.slope1 = 1.0f + 0.05f * a; // compander modules mistrack (the two outputs differently)
    comp_.slope2 = 1.0f - 0.025f * a;
    refBGain_ = 1.0f + 0.02f * a;                               // RA/RB trim: a DC thump under every crossfade
    xfStart_ = 512 - static_cast<int>(std::lround(160.0f * a)); // crossfade phase trim drifts: B's jump peeks through
    const float k = 0.25f + 1.75f * nl;                         // D/A bit-weight error, LSBs at the MSB
    for (int c = 0; c < 1024; ++c)
    {
        APP_FX_NOFUSE
        const float ideal = static_cast<float>(c) * (1.0f / 512.0f);
        lutA_[c] = ideal + k * errA_[c];
        lutB_[c] = ideal + k * errB_[c];
    }
}

void VintagePitch::applyParams() noexcept
{
    const auto moved = [&](int k)
    {
        if (applied_[k] == v_[k])
            return false;
        applied_[k] = v_[k];
        return true;
    };
    const bool pitch = moved(0);
    const bool fine = moved(1);
    if (pitch || fine)
        ratio_ = ratioOf(semitones(v_[0]), cents(v_[1]));
    if (moved(2))
        presetTarget_ =
            static_cast<int>(std::lround(static_cast<double>(delayMs(v_[2])) * 0.001 * static_cast<double>(kFrefHz)));
    if (moved(3))
        fbTarget_ = static_cast<float>(feedbackPct(v_[3])) * 0.01f * kFeedbackCeiling;
    if (moved(4))
        if (const int a = agePct(v_[4]); a != age_)
            setAge(a);
}

void VintagePitch::control() noexcept
{
    const float fs = static_cast<float>(fs_);
    const float fref = kFrefHz * (1.0f + clockDetune_);
    incRef_ = fref / fs;
    invIncRef_ = 1.0f / incRef_;
    drift_ = driftA_ * drift_ + driftB_ * ctlRng_.bipolar();
    fmPhase_ += fmInc_;
    if (fmPhase_ > 6.2831853f)
        fmPhase_ -= 6.2831853f;
    const float fm = 0.0003f * std::sin(fmPhase_) + 0.0001f * ctlRng_.bipolar();
    const float wobble = 1.0f + inst_ * (drift_ + fm);
    const float fin = std::clamp(ratio_ * fref * wobble, kRatioMin * fref, kRatioMax * fref);
    incIn_ = fin / fs;
    invIncIn_ = 1.0f / incIn_;
    jitter_ = inst_ * 30.0e-9f * fs; // CD4046 period jitter, in host samples
    // DELAY: the read tap jumps by the difference, hidden by a crossfade like any pointer jump — once the previous
    // crossfade is over, so a turning knob never restarts a running one.
    if (presetTarget_ != preset_ && xfCountdown_ < 0 && xfCount_ < 0)
    {
        readPos_ -= static_cast<std::uint32_t>(presetTarget_ - preset_);
        preset_ = presetTarget_;
        splice();
    }
}

// ------------------------------------------------------------------------------------------------ the digital board

void VintagePitch::splice() noexcept
{
    xfCountdown_ = xfStart_;
    ++splices_;
}

void VintagePitch::xfadeStep() noexcept // one LTCHB clock of the CD4020 window: a 0 → 1 → 0 staircase on REFA
{
    if (xfCountdown_ >= 0)
    {
        if (xfCountdown_ == 0)
            xfCount_ = 0;
        --xfCountdown_;
    }
    if (xfCount_ >= 0)
    {
        const int tri = xfCount_ < kXfadeHalf ? xfCount_ : kXfadeLength - xfCount_;
        refA_ = static_cast<float>((tri * xfSteps_) >> 9) * xfInvSteps_;
        if (++xfCount_ >= kXfadeLength)
            xfCount_ = -1;
    }
    else
        refA_ = 0.0f;
}

template <bool Neon> void VintagePitch::onFref(Chain<Neon>& c, float t) noexcept // A/D frame: convert, write
{
    APP_FX_NOFUSE
    using V = V2<Neon>;
    const V v = hermite(c.h0, c.h1, c.h2, c.h3, t);
    c.held = c.held + (v - c.held) * acq_;
    V u = c.held;
    u = u + (u * u - V::dup(0.5f)) * bow_;
    u = u * adcGain_ + V::dup(adcOffset_);
    const V code =
        (u * static_cast<float>(kAdcMid) + V::dup(static_cast<float>(kAdcMid))) + c.noise.bipolar() * noiseLsb_;
    V cf = vfloor(code);
    if ((lt(cf, V::dup(0.0f)) | gt(cf, V::dup(1023.0f))).any())
        ++blockLimits_; // the LIMIT LED
    cf = clamp2(cf, 0.0f, 1023.0f);
    ++w_;
    auto& word = ram_[w_ & kRamMask];
    cf.toCodes(codeMask_, word[0], word[1]);
    const int d = static_cast<int>(w_ - readPos_) - preset_;
    if (d > kWindow) // pitch down: a window behind → jump forward (one sample of hysteresis against the read side)
    {
        readPos_ += kWindow;
        splice();
    }
}

template <bool Neon> void VintagePitch::onFin(Chain<Neon>& c, float t) noexcept // pitch clock: read, swap, D/A
{
    APP_FX_NOFUSE
    using V = V2<Neon>;
    ++readPos_;
    const int d = static_cast<int>(w_ - readPos_) - preset_;
    if (d < 0) // pitch up: caught up with the write tap → jump back a window
    {
        readPos_ -= kWindow;
        splice();
    }
    const auto& a = ram_[readPos_ & kRamMask];
    auto& slot = echo_[e_];
    const std::int16_t bL = slot[0], bR = slot[1]; // tap B = tap A, 1024 events ago
    slot[0] = a[0];
    slot[1] = a[1];
    e_ = (e_ + 1u) & (kEchoWords - 1);
    xfadeStep();
    const float refB = (1.0f - refA_) * refBGain_;
    const V va = V::make(lutA_[a[0] & 1023], lutA_[a[1] & 1023]) * refA_;
    const V vb = V::make(lutB_[bL & 1023], lutB_[bR & 1023]) * refB;
    const V out = (va + vb) - V::dup(1.0f);
    float tq = t + jitter_ * jitRng_.bipolar();
    tq = std::clamp(tq, 0.0f, 0.9999f);
    tq = static_cast<float>(static_cast<std::int32_t>(tq * finGrid_)) * invFinGrid_; // re-timed to the system clock
    c.blep.step(out, tq);
}

template <> VintagePitch::Chain<false>& VintagePitch::chain<false>() noexcept
{
    return scalar_;
}
#if APP_FX_HAVE_NEON
template <> VintagePitch::Chain<true>& VintagePitch::chain<true>() noexcept
{
    return neon_;
}
#endif

template <bool Neon> void VintagePitch::run(float* left, float* right) noexcept
{
    APP_FX_NOFUSE
    using V = V2<Neon>;
    using namespace dsp::delay;
    constexpr int n = kBlockFrames;
    applyParams();
    control();
    auto& c = chain<Neon>();
    // S1: trim, feedback (the previous block's output, ramped to the knob)
    copy<Neon>(xL_, left, n);
    copy<Neon>(xR_, right, n);
    mul<Neon>(xL_, kTrim, n);
    mul<Neon>(xR_, kTrim, n);
    if (fbCur_ != 0.0f || fbTarget_ != 0.0f)
    {
        alignas(16) float g[n];
        const float step = (fbTarget_ - fbCur_) * (1.0f / n);
        for (int i = 0; i < n; ++i)
            g[i] = fbCur_ + step * static_cast<float>(i + 1);
        mulv<Neon>(fbL_, g, n);
        mulv<Neon>(fbR_, g, n);
        add<Neon>(xL_, fbL_, n);
        add<Neon>(xR_, fbR_, n);
    }
    fbCur_ = fbTarget_;

    // S2: compressor (pre-emphasis, 2:1), 4-pole anti-alias filter
    for (int i = 0; i < n; ++i)
    {
        if (i % kGainQuad == 0)
            c.comp.quad();
        V y = c.comp.process(V::load(xL_, xR_, i), comp_);
        y = c.aa2.process(c.aa1.process(y, aa1_), aa2_);
        y.store(xL_, xR_, i);
    }

    // S3: the digital board, events in time order inside each host sample
    blockLimits_ = 0;
    for (int i = 0; i < n; ++i)
    {
        c.h0 = c.h1;
        c.h1 = c.h2;
        c.h2 = c.h3;
        c.h3 = V::load(xL_, xR_, i);
        c.blep.begin();
        float tr = (1.0f - phRef_) * invIncRef_;
        float ti = (1.0f - phIn_) * invIncIn_;
        while (tr < 1.0f || ti < 1.0f)
        {
            if (tr <= ti)
            {
                onFref(c, tr);
                tr += invIncRef_;
            }
            else
            {
                onFin(c, ti);
                ti += invIncIn_;
            }
        }
        phRef_ = std::clamp(1.0f - (tr - 1.0f) * incRef_, 0.0f, 0.999999f);
        phIn_ = std::clamp(1.0f - (ti - 1.0f) * incIn_, 0.0f, 0.999999f);
        c.blep.acc.store(dL_, dR_, i);
    }
    limitHits_ += blockLimits_;
    limitHold_ = blockLimits_ > 0 ? 1.0f : limitHold_ * limitDecay_;

    // S4: reconstruction filter, expander (1:2, de-emphasis), output buffer → the next block's feedback
    for (int i = 0; i < n; ++i)
    {
        if (i % kGainQuad == 0)
            c.expand.quad(comp_);
        const V r = c.recon.process(V::load(dL_, dR_, i), recon_);
        const V o = c.out.process(c.expand.process(r, comp_), outCouplingR_, outMaxStep_);
        o.store(fbL_, fbR_, i);
    }

    // S5: make-up gain → wet
    copy<Neon>(left, fbL_, n);
    copy<Neon>(right, fbR_, n);
    mul<Neon>(left, kMakeup, n);
    mul<Neon>(right, kMakeup, n);
}

void VintagePitch::processScalar(float* left, float* right) noexcept
{
    run<false>(left, right);
}

#if APP_FX_HAVE_NEON
void VintagePitch::processNeon(float* left, float* right) noexcept
{
    run<true>(left, right);
}
#endif

void VintagePitch::process(float* left, float* right, const ProcessContext&) noexcept
{
#if APP_FX_HAVE_NEON
    run<true>(left, right);
#else
    run<false>(left, right);
#endif
}
} // namespace engine::fx

// Registration: every slot kind (a harmonizer on a send is the classic doubler), the shell's input fade, no wet
// high-pass (the unit has its own coupling capacitors), MIX = dry 1 − v / wet v.
APP_REGISTER_EFFECT((engine::fx::EffectDescriptor{
    .id = "vpitch",
    .name = "VINTAGE PITCH",
    .slots = static_cast<std::uint8_t>(engine::fx::SlotKind::Insert | engine::fx::SlotKind::Send) |
             static_cast<std::uint8_t>(engine::fx::SlotKind::Master),
    .params = {"fx.vpitch.pitch", "fx.vpitch.fine", "fx.vpitch.delay", "fx.vpitch.feedback", "fx.vpitch.age",
               "fx.vpitch.mix"},
    .options = {},
    .visual = "pitchSplice",
    .shell = {.gateMode = static_cast<std::uint8_t>(engine::fx::GateMode::InputFade),
              .mixLaw = static_cast<std::uint8_t>(engine::fx::MixLaw::CrossfadeK5),
              .wetHighPass = false,
              .skipWhenSilent = false,
              .clampOutput = false},
    .create = &engine::fx::make<engine::fx::VintagePitch>,
    .featureFlag = {}}))
