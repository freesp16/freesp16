// Insert Delay — see InsertDelay.h. Every float32 step is one C++ float operation (never fused: APP_FX_NOFUSE on the
// host's clang, -ffp-contract=off / -fno-unsafe-math-optimizations on GCC); tan/pow/log10 in double only at hi-cut
// steps and in prepare(). Addresses in comments are the reference's (research docs/effects/insert-delay.md "1.60 code
// map").
#include "engine/fx/effects/InsertDelay.h"

#include "engine/fx/EffectRegistryOrder.h"
#include "engine/fx/FxShell.h"

#include <cmath>

namespace engine::fx
{
namespace
{
using namespace dsp::delay;

struct Beats
{
    int num, den;
};
// 1/8, 1/4, 1/2, 3/4, 1, 2 beats (index rint(5v)); index 6 = the table reader's out-of-range fallback 64/1
constexpr Beats kSyncBeats[7] = {{1, 8}, {1, 4}, {1, 2}, {3, 4}, {1, 1}, {2, 1}, {64, 1}};
} // namespace

float InsertDelay::freeMs(float v) noexcept
{
    return static_cast<float>(rintIdx(10, 1001, v));
}

float InsertDelay::syncMs(float v, float bpm) noexcept
{
    APP_FX_NOFUSE
    int i = rintIdx(0, 6, v);
    if (i < 0 || i > 5)
        i = 6;
    const auto [a, b] = kSyncBeats[i];
    const float ms = (((static_cast<float>(a) / static_cast<float>(b)) * 1000.0f) * 60.0f) / bpm;
    if (ms < 10.0f)
        return 10.0f;
    return ms > 1000.0f ? 1000.0f : ms;
}

float InsertDelay::feedbackGain(float v) noexcept
{
    APP_FX_NOFUSE
    return static_cast<float>(rintIdx(0, 101, v)) * f32(0.01);
}

std::uint32_t InsertDelay::hiCutStepOf(float v) noexcept
{
    APP_FX_NOFUSE
    return truncU32(v * 999.0f) + 1u;
}

void InsertDelay::prepare(double sampleRate, int)
{
    fs_ = sampleRate;
    bytes_ = mod_.prepare(sampleRate, 1000.0f);
    fb_.prepareSamples(sampleRate, rampSamples(sampleRate));
    hiCut_.prepare(SteppedIir1::Kind::LowPass, sampleRate, 1000, 2000.0, 20000.0);
    init();
}

void InsertDelay::init() noexcept // the constructor 0x103488
{
    constexpr float kCtor[6] = {0.75f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f};
    for (int k = 0; k < 6; ++k)
        p_[k].init(kCtor[k]);
    sync_.init(0);
    bpm_.init(120.0f);
    syncV_ = 0;
    bpmV_ = 120.0f;
    timeV_ = 0.5f;
    dirty_ = false;
    mod_.init();
    fb_.snap(0.0f);
    hiCut_.init();
}

void InsertDelay::setParam(int index, float value01) noexcept // slot 14: store only
{
    if (index >= 0 && index < 6)
        p_[index].set(value01);
}

void InsertDelay::setOption(int which, int value) noexcept
{
    if (which == kOptSync)
        sync_.set(static_cast<float>(value));
    else if (which == kClearState && value != 0)
        init();
}

void InsertDelay::applyParams() noexcept // slot 32 0x102f30: SYNC → BPM → TIME → FEEDBACK → HI CUT
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
    if (dirty_)
    {
        mod_.setDelay(syncV_ != 0 ? syncMs(timeV_, bpmV_) : freeMs(timeV_));
        dirty_ = false;
    }
    if (p_[1].changed())
        fb_.setTarget(feedbackGain(p_[1].get()));
    if (p_[2].changed())
        hiCut_.setStep(hiCutStepOf(p_[2].get()));
}

void InsertDelay::reset() noexcept // slot 27 0x102d04: slot 32, then 0x103d20 (slot 32 again, module reset, ramps)
{
    if ((quirks_ & kDelayQuirkCtorTime) == 0) // Sixteen (FX-682 fix, FX-056): the stored values always apply
    {
        for (auto& p : p_)
            p.untrap();
        sync_.untrap();
        bpm_.untrap();
    }
    applyParams();
    applyParams();
    const bool stale = (quirks_ & kDelayQuirkStaleRing) != 0;
    mod_.reset(stale, (quirks_ & kDelayQuirkWarmUp) != 0);
    if (! stale) // Sixteen (FX-681): no pre-reset audio, so the hi-cut's state goes with the ring (FX-660 keeps it)
        hiCut_.zeroState();
    warmUp_ = (quirks_ & kDelayQuirkWarmUp) != 0;
    fb_.snap(fb_.target());
}

template <bool Neon> void InsertDelay::run(float* left, float* right, float bpm) noexcept // slot 25 0x102cc0
{
    bpm_.set(bpm); // the processor's tempo feed (parameter 7), consumed by the change detection
    applyParams();
    constexpr int n = kBlockFrames;
    int pos = 0;
    while (n - pos > 0) // slot 33 0x102d3c: one chunk per block (D >= 441), split only inside the module's read
    {
        const int ch = mod_.chunk();
        const int c = ch < n - pos ? ch : n - pos;
        float* y[2] = {yL_ + pos, yR_ + pos};
        mod_.read<Neon>(y, c, warmUp_);
        hiCut_.process<Neon>(yL_ + pos, yR_ + pos, c);
        copy<Neon>(wL_ + pos, yL_ + pos, c);
        copy<Neon>(wR_ + pos, yR_ + pos, c);
        smoothMul<Neon>(fb_, wL_ + pos, wR_ + pos, c); // × g (147-sample ramp)
        add<Neon>(wL_ + pos, left + pos, c);           // in + g·y
        add<Neon>(wR_ + pos, right + pos, c);
        const float* w[2] = {wL_ + pos, wR_ + pos};
        mod_.write(w, c);
        pos += c;
    }
    copy<Neon>(left, yL_, n); // wet only
    copy<Neon>(right, yR_, n);
}

void InsertDelay::processScalar(float* left, float* right, float bpm) noexcept
{
    run<false>(left, right, bpm);
}

#if APP_FX_HAVE_NEON
void InsertDelay::processNeon(float* left, float* right, float bpm) noexcept
{
    run<true>(left, right, bpm);
}
#endif

void InsertDelay::process(float* left, float* right, const ProcessContext& ctx) noexcept
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
constexpr std::string_view kInsertDelayOptions[] = {"fx.delay.sync"};
} // namespace

// Registration: Insert only (FX-040), gate mode 1 (FX-625), no wet high-pass, the piecewise
// delay mix law of k5 (FX-624).
APP_REGISTER_EFFECT((engine::fx::EffectDescriptor{
    .id = "delay",
    .name = "DELAY",
    .slots = static_cast<std::uint8_t>(engine::fx::SlotKind::Insert),
    .params = {"fx.delay.time", "fx.delay.feedback", "fx.delay.hiCut", "", "", "fx.delay.mix"},
    .options = kInsertDelayOptions,
    .visual = "delayTaps",
    .shell = {.gateMode = static_cast<std::uint8_t>(engine::fx::GateMode::InputFade),
              .mixLaw = static_cast<std::uint8_t>(engine::fx::MixLaw::DelayPiecewise),
              .wetHighPass = false,
              .skipWhenSilent = false,
              .clampOutput = false},
    .create = &engine::fx::make<engine::fx::InsertDelay>,
    .featureFlag = {}}))
