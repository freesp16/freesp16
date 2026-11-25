// Send/Master Delay — see SendDelay.h. Float steps one C++ float operation each, never fused (APP_FX_NOFUSE); libm in
// double only at filter steps and in prepare(). Addresses are the reference's (research docs/effects/send-delay.md).
#include "engine/fx/effects/SendDelay.h"

#include "engine/fx/EffectRegistryOrder.h"
#include "engine/fx/FxShell.h"
#include "engine/fx/effects/InsertDelay.h"

namespace engine::fx
{
namespace
{
using namespace dsp::delay;

struct Beats
{
    int num, den;
};
// 1/8, 1/4, 1/2, 3/4, 1, 2, 4, 8 beats (index rint(7v); out of range → index 6, the table reader 0x34a498)
constexpr Beats kSyncBeats[8] = {{1, 8}, {1, 4}, {1, 2}, {3, 4}, {1, 1}, {2, 1}, {4, 1}, {8, 1}};
constexpr float kMaxMs = 4000.0f;
} // namespace

float SendDelay::freeMs(float v) noexcept
{
    return static_cast<float>(rintIdx(10, 4001, v));
}

float SendDelay::syncMs(float v, float bpm) noexcept
{
    APP_FX_NOFUSE
    int i = rintIdx(0, 8, v);
    if (i < 0 || i > 7)
        i = 6;
    const auto [a, b] = kSyncBeats[i];
    const float ms = (((static_cast<float>(a) / static_cast<float>(b)) * 1000.0f) * 60.0f) / bpm;
    if (ms < 10.0f)
        return 10.0f;
    return kMaxMs < ms ? kMaxMs : ms;
}

std::uint32_t SendDelay::hiCutStepOf(float v) noexcept
{
    APP_FX_NOFUSE
    return static_cast<std::uint32_t>(truncS32(v * 999.0f)) + 1u;
}

std::uint32_t SendDelay::loCutStepOf(float v) noexcept
{
    APP_FX_NOFUSE
    const float x = 1.0f - v;
    const float x4 = x * (x * (x * x));
    return static_cast<std::uint32_t>(truncS32((1.0f - x4) * 999.0f)) + 1u;
}

void SendDelay::prepare(double sampleRate, int)
{
    fs_ = sampleRate;
    colour_.prepare(sampleRate);
    bytes_ = modL_.prepare(sampleRate, kMaxMs) + modR_.prepare(sampleRate, kMaxMs);
    const int n = rampSamples(sampleRate);
    fbL_.prepareSamples(sampleRate, n);
    fbR_.prepareSamples(sampleRate, n);
    direct_.prepareSamples(sampleRate, n);
    cross_.prepareSamples(sampleRate, n);
    hiCut_.prepare(SteppedIir1::Kind::LowPass, sampleRate, 1000, 2000.0, 20000.0);
    loCut_.prepare(SteppedIir1::Kind::HighPass, sampleRate, 1000, 1.0, 1000.0);
    init();
}

void SendDelay::init() noexcept // the constructor 0x1054ac
{
    constexpr float kCtor[6] = {0.5f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f};
    for (int k = 0; k < 6; ++k)
        p_[k].init(kCtor[k]);
    sync_.init(0);
    colour_.reset(false);
    bpm_.init(120.0f);
    baseTL_ = baseTR_ = 1.0f;
    baseGL_ = baseGR_ = 0.0f;
    syncV_ = 0;
    bpmV_ = 120.0f;
    timeV_ = 0.5f;
    dirty_ = false;
    modL_.init();
    modR_.init();
    fbL_.snap(0.0f);
    fbR_.snap(0.0f);
    direct_.snap(1.0f);
    cross_.snap(0.0f);
    hiCut_.init();
    loCut_.init();
}

void SendDelay::setParam(int index, float value01) noexcept
{
    if (index >= 0 && index < 6)
        p_[index].set(value01);
}

void SendDelay::setOption(int which, int value) noexcept
{
    if (which == kOptSync)
        sync_.set(static_cast<float>(value));
    else if (which == kOptMode)
        colour_.set(value == 1);
    else if (which == kClearState && value != 0)
        init();
}

void SendDelay::applyParams() noexcept
{
    if (sync_.changed())
    {
        syncV_ = sync_.get();
        dirty_ = true;
    }
    if (bpm_.changed())
    {
        bpmV_ = bpm_.get();
        dirty_ = true;
    }
    if (p_[0].changed())
    {
        timeV_ = p_[0].get();
        dirty_ = true;
    }
    if (dirty_) // the same time into DelayTimeLch and DelayTimeRch (F117: no separate L/R times, FX-763)
    {
        const float ms = syncV_ != 0 ? syncMs(timeV_, bpmV_) : freeMs(timeV_);
        baseTL_ = baseTR_ = ms;
        dirty_ = false;
    }
    baseGL_ = baseGR_ = p_[1].get(); // unquantised (F116)
    hiCut_.setStep(hiCutStepOf(p_[2].get()));
    loCut_.setStep(loCutStepOf(p_[3].get()));
}

void SendDelay::applyBase() noexcept
{
    APP_FX_NOFUSE
    const float c = p_[4].get(); // Spread, read directly by the base
    if (! dsp::nearlyEqual(baseTL_, modL_.ms()))
        modL_.setDelay(baseTL_);
    if (! dsp::nearlyEqual(baseGL_, fbL_.target()))
        fbL_.setTarget(baseGL_);
    if (! dsp::nearlyEqual(baseTR_, modR_.ms()))
        modR_.setDelay(baseTR_);
    if (! dsp::nearlyEqual(baseGR_, fbR_.target()))
        fbR_.setTarget(baseGR_);
    direct_.setTarget(1.0f - c);
    cross_.setTarget(c);
}

void SendDelay::reset() noexcept // slot 27 0x1049fc → 0x104a04: slot 8, then 0x104108 (slot 8, module resets, ramps)
{
    if ((quirks_ & kDelayQuirkCtorTime) == 0) // Sixteen (FX-782 fix, FX-056): the stored values apply first
    {
        for (auto& p : p_)
            p.untrap();
        sync_.untrap();
        bpm_.untrap();
        applyParams();
    }
    applyBase();
    applyBase();
    colour_.reset(colour_.tape());
    const bool referenceTime = (quirks_ & kDelayQuirkGatedTime) != 0;
    const bool stale = (quirks_ & kDelayQuirkStaleRing) != 0;
    warmUp_ = (quirks_ & kDelayQuirkWarmUp) != 0;
    modL_.reset(stale, warmUp_, referenceTime);
    fbL_.snap(fbL_.target());
    modR_.reset(stale, warmUp_, referenceTime);
    fbR_.snap(fbR_.target());
    direct_.snap(direct_.target());
    cross_.snap(cross_.target());
    if (! stale) // Sixteen (FX-781): no pre-reset audio, the filter states go with the rings (FX-761 keeps them)
    {
        hiCut_.zeroState();
        loCut_.zeroState();
    }
}

template <bool Neon> void SendDelay::run(float* left, float* right, float bpm) noexcept // slot 25 0x104e90
{
    bpm_.set(bpm);
    applyParams();
    applyBase(); // the core 0x104a30 starts with slot 8
    constexpr int n = kBlockFrames;
    int pos = 0;
    while (n - pos > 0) // chunks end where the left line's read would pass its delay (F117)
    {
        const int ch = modL_.chunk();
        const int c = ch < n - pos ? ch : n - pos;
        float* a = s0_ + pos;
        float* b = s1_ + pos;
        float* l1[1] = {a};
        float* r1[1] = {b};
        modL_.read<Neon>(l1, c, warmUp_, colour_.tape());
        modR_.read<Neon>(r1, c, warmUp_, colour_.tape());
        float* xa = s5_ + pos; // cross · (yL, yR)
        float* xb = s4_ + pos;
        copy<Neon>(xa, a, c);
        copy<Neon>(xb, b, c);
        smoothMul<Neon>(cross_, xa, xb, c);
        smoothMul<Neon>(direct_, a, b, c); // direct · (yL, yR)
        add<Neon>(a, xb, c);               // L' = d·yL + x·yR
        add<Neon>(b, xa, c);               // R' = d·yR + x·yL
        hiCut_.process<Neon>(a, b, c);
        loCut_.process<Neon>(a, b, c);
        colour_.process(a, b, c);
        float* fa = s2_ + pos;
        float* fb = s3_ + pos;
        copy<Neon>(fa, a, c);
        copy<Neon>(fb, b, c);
        smoothMul<Neon>(fbL_, fa, nullptr, c);
        smoothMul<Neon>(fbR_, fb, nullptr, c);
        add<Neon>(fa, left + pos, c);
        add<Neon>(fb, right + pos, c);
        const float* wl[1] = {fa};
        const float* wr[1] = {fb};
        modL_.write(wl, c);
        modR_.write(wr, c);
        pos += c;
    }
    copy<Neon>(left, s0_, n); // wet only: (L', R') after both filters
    copy<Neon>(right, s1_, n);
}

void SendDelay::processScalar(float* left, float* right, float bpm) noexcept
{
    run<false>(left, right, bpm);
}

#if APP_FX_HAVE_NEON
void SendDelay::processNeon(float* left, float* right, float bpm) noexcept
{
    run<true>(left, right, bpm);
}
#endif

void SendDelay::process(float* left, float* right, const ProcessContext& ctx) noexcept
{
    const float bpm = static_cast<float>(ctx.bpm);
#if APP_FX_HAVE_NEON
    run<true>(left, right, bpm);
#else
    run<false>(left, right, bpm);
#endif
}
} // namespace engine::fx

namespace
{
constexpr std::string_view kSendDelayOptions[] = {"fx.delayBus.sync", "fx.delayBus.type1"};
} // namespace

// Registration: Send and Master (FX-040, index 0 of both lists), gate mode 1 (FX-725), no wet
// high-pass (F141), the piecewise delay mix law of k5.
APP_REGISTER_EFFECT((engine::fx::EffectDescriptor{
    .id = "delayBus",
    .name = "DELAY",
    .slots = static_cast<std::uint8_t>(engine::fx::SlotKind::Send | engine::fx::SlotKind::Master),
    .params = {"fx.delayBus.time", "fx.delayBus.feedback", "fx.delayBus.hiCut", "fx.delayBus.loCut",
               "fx.delayBus.spread", "fx.delayBus.mix"},
    .options = kSendDelayOptions,
    .visual = "delayTaps",
    .shell = {.gateMode = static_cast<std::uint8_t>(engine::fx::GateMode::InputFade),
              .mixLaw = static_cast<std::uint8_t>(engine::fx::MixLaw::DelayPiecewise),
              .wetHighPass = false,
              .skipWhenSilent = false,
              .clampOutput = false},
    .create = &engine::fx::make<engine::fx::SendDelay>,
    .featureFlag = {}}))
