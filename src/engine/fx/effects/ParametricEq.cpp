// 2-band EQ — see ParametricEq.h. Every routine names the reference routine (research docs/effects/parametric-eq.md
// "1.60 code map") and mirrors tools/fx/peq.py line for line.
#include "engine/fx/effects/ParametricEq.h"

#include "engine/fx/EffectRegistryOrder.h"
#include "engine/fx/FxShell.h"
#include "engine/fx/dsp/filter_fdlibm.h"

#include <cmath>

namespace engine::fx::effects
{
namespace
{
using dsp::filt::rnd;

// Constructor "last" values (0x10baec): the six floats 0.5, both types 5.
constexpr float kCtorValues[8] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 5.0f, 5.0f};
constexpr float kMinFc = 5.0f;          // IirFilter stages only
constexpr int kTypeDefaults[2] = {0, 1}; // band 1 low shelf, band 2 high shelf

int clampType(int t) noexcept
{
    return t < 0 ? 0 : (t > 4 ? 4 : t);
}
} // namespace

// ------------------------------------------------------------------------------------------------ parameter laws

float ParametricEq::freqOf(float v, bool parityMaths) noexcept
{
    APP_FX_NOFUSE
    const float p = parityMaths ? dsp::fdlibm::powf(1000.0f, v)
                                : static_cast<float>(std::pow(1000.0, static_cast<double>(v)));
    return p * 20.0f;
}
float ParametricEq::gainDbOf(float v) noexcept
{
    APP_FX_NOFUSE
    return v * 45.0f - 30.0f;
}
float ParametricEq::qOf(float v) noexcept
{
    APP_FX_NOFUSE
    return 0.1f + v * 9.9f;
}
float ParametricEq::gainOf(float gdb) noexcept
{
    APP_FX_NOFUSE
    return static_cast<float>(std::pow(10.0, static_cast<double>(gdb * 0.05f)));
}

// ------------------------------------------------------------------------------------------------ life cycle

void ParametricEq::prepare(double sampleRate, int maxBlock)
{
    (void) maxBlock; // the engine block (64) is the only size process() handles
    fs_ = sampleRate;
    clear();
}

void ParametricEq::clear() noexcept
{
    APP_FX_NOFUSE
    // the static initialiser 0x13aa8: powf(10, 0.012) = 1000^(1/250) and powf(10, 0.02), and their inverses
    mF_ = dsp::fdlibm::powf(10.0f, 0.012f);
    mFi_ = 1.0f / mF_;
    mQ_ = dsp::fdlibm::powf(10.0f, 0.02f);
    mQi_ = 1.0f / mQ_;
    const int ifs = static_cast<int>(fs_);
    for (auto& b : bands_)
    {
        for (int k = 0; k < 5; ++k)
        {
            Stage& s = b.st[k];
            s.kind = k;
            s.shelf.init(k == 2, static_cast<double>(ifs));
            s.biquad = {};
            s.nyq = static_cast<float>(static_cast<int>(static_cast<double>(ifs >= 10 ? ifs : 10) / 2.0));
            s.fc = (s.nyq + 5.0f) * 0.5f;
            s.Q = 0.7071067811865476;
        }
        b.gate.init(dsp::filt::gateSamples(fs_));
        b.gain.init(44, 1.0f);
        b.pending = b.cur = 0;
        b.ft = b.fc = 1000.0f;
        b.fmult = mF_;
        b.fsteps = 0;
        b.qt = b.qc = 0.7f;
        b.qmult = mQ_;
        b.qsteps = 0;
        b.gt = b.gc = 0.0f;
        b.gstep = 0.04f;
        b.gsteps = 0;
        b.next = 0.0f;
        b.maxsteps = 0;
        b.flag60 = b.flag61 = b.dirty62 = false;
    }
    for (int i = 0; i < 8; ++i)
    {
        p_[i].init(i >= 6, kCtorValues[i]);
        userSet_[i] = false;
    }
    pristine_ = true;
    armQuirks();
}

void ParametricEq::armQuirks() noexcept // see MultiModeFilter::armQuirks (FX-056)
{
    const bool trap = (quirks_ & kQuirkChangeTrap) != 0;
    for (int i = 0; i < 8; ++i)
    {
        auto& p = p_[i];
        if (! userSet_[i])
        {
            const float v = trap ? kCtorValues[i] : (i < 6 ? kDefaults[i] : static_cast<float>(kTypeDefaults[i - 6]));
            p.curF = v;
            p.curI = static_cast<int>(v);
        }
        p.lastF = kCtorValues[i];
        p.lastI = static_cast<int>(kCtorValues[i]);
        p.untrapped = ! trap;
    }
}

void ParametricEq::setQuirkProfile(std::uint32_t mask) noexcept
{
    quirks_ = mask;
    if (pristine_)
        armQuirks();
}

void ParametricEq::setParam(int index, float value01) noexcept
{
    if (index < 0 || index >= 6)
        return;
    p_[index].set(value01);
    userSet_[index] = true;
}

void ParametricEq::setOption(int which, int value) noexcept
{
    if (which == kOptType1 || which == kOptType2)
    {
        p_[6 + which].setChoice(4 - clampType(value)); // UI-order enum → the reference's band type
        userSet_[6 + which] = true;
    }
    else if (which == kOptParityMaths)
        parityMaths_ = value != 0;
    else if (which == kClearState && value != 0)
        clear();
}

// slot 27 (0x10a7ec)
void ParametricEq::reset() noexcept
{
    pristine_ = false;
    applyParams();
    for (auto& b : bands_)
    {
        Stage& s = b.st[b.cur];
        if (s.kind <= kPeak)
            s.shelf.iir.clearTaps();
        else
            s.biquad.clearTaps();
    }
}

// ------------------------------------------------------------------------------------------------ parameters

// 0x10a4f0: B1F, B1G, B1Q, B2F, B2G, B2Q, TYPE1, TYPE2 (the setters inline)
void ParametricEq::applyParams() noexcept
{
    for (int bi = 0; bi < 2; ++bi)
    {
        Band& b = bands_[bi];
        const int base = 3 * bi;
        if (p_[base].changed())
        {
            b.ft = freqOf(p_[base].get(), parityMaths_);
            b.dirty62 = true;
            b.flag61 = b.fsteps == 0;
        }
        if (p_[base + 1].changed())
        {
            b.flag61 = b.gsteps == 0 ? true : b.flag61;
            b.dirty62 = true;
            b.gt = gainDbOf(p_[base + 1].get());
        }
        if (p_[base + 2].changed())
        {
            b.flag61 = b.qsteps == 0 ? true : b.flag61;
            b.dirty62 = true;
            b.qt = qOf(p_[base + 2].get());
        }
    }
    for (int bi = 0; bi < 2; ++bi)
        if (p_[6 + bi].changed())
        {
            Band& b = bands_[bi];
            const int t = clampType(rnd(p_[6 + bi].get()));
            b.pending = t;
            if (b.cur != t)
                b.gate.close();
        }
}

// Stage configure: step path (0x10c3a4…) and swap path (0x10c0f0…)
void ParametricEq::configure(Band& b, Stage& s, float fc, float qc, float gc, bool flag, bool swap) noexcept
{
    APP_FX_NOFUSE
    const int k = s.kind;
    if (k == kLowShelf || k == kPeak)
    {
        s.shelf.configure(fc, static_cast<double>(qc), static_cast<double>(gc), flag);
        b.gain.setTarget(1.0f);
        return;
    }
    if (k == kHighShelf)
        s.shelf.configure(fc, static_cast<double>(qc), -static_cast<double>(gc), flag);
    else
    {
        float v = s.nyq;
        if (fc <= v)
            v = fc;
        float c = kMinFc;
        if (c <= v)
            c = v;
        s.Q = static_cast<double>(qc);
        s.fc = c;
        const int ifs = static_cast<int>(fs_);
        s.biquad.c = dsp::filt::bilinear(k == kLowPass ? dsp::filt::Kind::LowPass : dsp::filt::Kind::HighPass, c, s.Q,
                                         static_cast<double>(ifs >= 10 ? ifs : 10));
        s.biquad.reseed = flag;
    }
    const float g = gainOf(gc);
    if (swap)
        b.gain.setNow(g);
    else
        b.gain.setTarget(g);
}

// 0x10abdc
void ParametricEq::rampSetup(Band& b) noexcept
{
    APP_FX_NOFUSE
    using dsp::fdlibm::log10f;
    if (b.dirty62)
    {
        b.fsteps = static_cast<int>(dsp::absf((1.0f / log10f(mF_)) * log10f(b.ft / b.fc))) + 1;
        b.fmult = b.ft > b.fc ? mF_ : mFi_;
        b.qsteps = static_cast<int>(dsp::absf((1.0f / log10f(mQ_)) * log10f(b.qt / b.qc))) + 1;
        b.qmult = b.qt > b.qc ? mQ_ : mQi_;
        b.next = 44.1f;
        b.dirty62 = false;
        b.gsteps = static_cast<int>(dsp::absf(b.gt - b.gc) * 25.0f) + 1;
        b.gstep = b.gt > b.gc ? 0.04f : -0.04f;
    }
    int m = b.fsteps > b.qsteps ? b.fsteps : b.qsteps;
    b.maxsteps = m > b.gsteps ? m : b.gsteps;
    if (b.fsteps > 38 || b.qsteps > 70 || b.gsteps > 250)
        b.gate.close(); // FX-425 gated jump
}

void ParametricEq::step(Band& b) noexcept
{
    APP_FX_NOFUSE
    if (b.fsteps > 1)
    {
        b.fsteps -= 1;
        b.fc = b.fc * b.fmult;
    }
    else
    {
        b.fsteps = 0;
        b.fc = b.ft;
    }
    if (b.qsteps > 1)
    {
        b.qsteps -= 1;
        b.qc = b.qc * b.qmult;
    }
    else
    {
        b.qsteps = 0;
        b.qc = b.qt;
    }
    if (b.gsteps > 1)
    {
        b.gsteps -= 1;
        b.gc = b.gc + b.gstep;
    }
    else
    {
        b.gsteps = 0;
        b.gc = b.gt;
    }
}

// 0x10c070
void ParametricEq::swapType(Band& b) noexcept
{
    b.fsteps = b.qsteps = b.gsteps = b.maxsteps = 0;
    b.fc = b.ft;
    b.qc = b.qt;
    b.gc = b.gt;
    b.cur = b.pending;
    configure(b, b.st[b.cur], b.ft, b.qt, b.gt, true, true);
    b.gate.open();
}

// ------------------------------------------------------------------------------------------------ processing

template <bool Neon> void ParametricEq::runStage(Band& b, Stage& s, float* l, float* r, int n) noexcept
{
    if (s.kind <= kPeak)
        dsp::filt::iirRun<Neon>(s.shelf.iir, s.shelf.Q > 0.0, l, r, n);
    else
        dsp::filt::iirRun<Neon>(s.biquad, s.Q > 0.0, l, r, n);
    if (n > 0)
        b.gain.process<Neon>(l, r, n);
}

// 0x10c208
template <bool Neon> void ParametricEq::runBand(Band& b, float* l, float* r, int n) noexcept
{
    APP_FX_NOFUSE
    rampSetup(b);
    if (b.gate.r.state == 1)
    {
        if (b.gate.fadingOut()) // a type switch or a gated jump
        {
            const int rem = b.gate.r.rem;
            if (n < rem)
            {
                runStage<Neon>(b, b.st[b.cur], l, r, n);
                b.gate.apply<Neon>(l, r, n);
                return;
            }
            runStage<Neon>(b, b.st[b.cur], l, r, rem);
            b.gate.apply<Neon>(l, r, rem);
            swapType(b);
            runStage<Neon>(b, b.st[b.cur], l + rem, r + rem, n - rem);
            b.gate.apply<Neon>(l + rem, r + rem, n - rem);
            return;
        }
    }
    else if (dsp::nearlyEqual(b.gate.r.cur, dsp::filt::RefGate::kOff)) // closed: only a freshly built band
    {
        swapType(b);
        if ((quirks_ & kQuirkStartupGate) != 0)
        {
            b.gate.apply<Neon>(l, r, n); // FX-480: this block is not equalised and gets no output gain
            return;
        }
        b.gate.setNow(true); // Sixteen: open from the first sample, the stage runs from block 0
    }
    int rem = n, pos = 0;
    while (rem > 0)
    {
        int chunk;
        if (b.maxsteps <= 0)
        {
            chunk = rem;
            rem = 0;
        }
        else
        {
            if (b.flag61) // the first step is taken at once after a parameter change
            {
                step(b);
                configure(b, b.st[b.cur], b.fc, b.qc, b.gc, false, false);
                b.flag61 = false;
            }
            const float s15 = b.next;
            const int r8 = rnd(s15);
            if (rem < r8)
            {
                chunk = rem;
                b.next = s15 - static_cast<float>(rem);
                rem = 0;
            }
            else
            {
                chunk = r8;
                rem -= r8;
                b.next = (s15 - static_cast<float>(r8)) + 44.1f;
                step(b);
                b.flag60 = true;
            }
        }
        Stage& s = b.st[b.cur];
        runStage<Neon>(b, s, l + pos, r + pos, chunk);
        pos += chunk;
        if (b.flag60)
        {
            configure(b, s, b.fc, b.qc, b.gc, false, false);
            b.flag60 = false;
        }
    }
    b.gate.apply<Neon>(l, r, n);
}

// slot 25 (0x10ae70); slot 26 (dry 0, wet 1) is the shell's WetOnly law
template <bool Neon> void ParametricEq::run(float* l, float* r) noexcept
{
    pristine_ = false;
    applyParams();
    runBand<Neon>(bands_[0], l, r, kBlockFrames);
    runBand<Neon>(bands_[1], l, r, kBlockFrames);
}

void ParametricEq::process(float* left, float* right, const ProcessContext&) noexcept
{
    run<APP_FX_HAVE_NEON != 0>(left, right);
}

void ParametricEq::processScalar(float* left, float* right, const ProcessContext&) noexcept
{
    run<false>(left, right);
}

ParametricEq::BandView ParametricEq::band(int bi) const noexcept
{
    const Band& b = bands_[bi & 1];
    return {b.cur, b.pending, b.fsteps, b.qsteps, b.gsteps, b.fc, b.qc, b.gc, b.gain.cur};
}

const dsp::filt::Coefs& ParametricEq::coefs(int bi) const noexcept
{
    const Band& b = bands_[bi & 1];
    const Stage& s = b.st[b.cur];
    return s.kind <= kPeak ? s.shelf.iir.c : s.biquad.c;
}
} // namespace engine::fx::effects

namespace
{
constexpr std::string_view kEq2Options[] = {"fx.eq2.type1", "fx.eq2.type2"};
}

// Page order (parameters.md FX-067): B1 Q, B1 Freq, B1 Gain, B2 Q, B2 Freq, B2 Gain; the knob → internal index map is
// the catalogue's (core/params/catalogue/fx_eq2.cpp).
APP_REGISTER_EFFECT((engine::fx::EffectDescriptor{
    .id = "eq2",
    .name = "2 BAND EQ",
    .slots = engine::fx::kAllSlotKinds,
    .params = {"fx.eq2.b1q", "fx.eq2.b1freq", "fx.eq2.b1gain", "fx.eq2.b2q", "fx.eq2.b2freq", "fx.eq2.b2gain"},
    .options = kEq2Options,
    .visual = "eqCurve",
    // FX-461: dry 0, wet 1, no wet high-pass, gate open, no skip/clamp
    .shell = {.gateMode = static_cast<std::uint8_t>(engine::fx::GateMode::Open),
              .mixLaw = static_cast<std::uint8_t>(engine::fx::MixLaw::WetOnly),
              .wetHighPass = false,
              .skipWhenSilent = false,
              .clampOutput = false},
    .create = &engine::fx::make<engine::fx::effects::ParametricEq>,
    .featureFlag = {}}))
