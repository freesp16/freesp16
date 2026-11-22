// Send/Master Delay ("delayBus"; spec 02-features/effects/send-delay.md FX-720…725, FX-760…763, quirks FX-780…784;
// research docs/effects/send-delay.md F080, F115–F120, CC BY 4.0). Operation for operation as the MIT model
// tools/fx/send_delay.py (class SendDelay, pinned commit 8901c97) in the Reference quirk profile, so
// tests/golden/delayBus matches it through the shell with exact integer delay positions.
//
//   per 64-frame block:  BPM from ProcessContext → change detection on SYNC, BPM, TIME only (FX-760); feedback,
//                        hi cut, lo cut, spread and mix re-read every block
//                        → two mono lines with one shared integer D (rings trunc(4 s·fs) + 2500, FX-720), each read
//                          through its own 147-sample gate (time change: fade out, jump, fade in, FX-724)
//                        → spread matrix L' = d·yL + x·yR, R' = d·yR + x·yL (d → 1 − s, x → s, 147-sample ramps,
//                          FX-721)
//                        → HiCut LP (2–20 kHz) → LowCut HP (1–1000 Hz, 5 Hz floor), stepped first-order (FX-722)
//                        → wet = (L', R');  line_L ← inL + g·L', line_R ← inR + g·R' (FX-723)
//   the shell (descriptor): gate mode 1, no wet high-pass, the piecewise delay mix law of k5 (FX-725).
//
// Knobs k: 0 time (free ms = rint(10 + 3990v); SYNC i = rint(7v) of 1/8 … 8 beats, clamped to 10…4000 ms), 1 feedback
// g = v (unquantised; the shell clamps v to [0, 1], FX-783), 2 hi cut, 3 lo cut, 4 spread, 5 mix (the shell's).
// Option 0 = SYNC ("fx.delayBus.sync").
//
// Quirk profile (the Insert Delay's bits, InsertDelay.h): kDelayQuirkWarmUp FX-780, kDelayQuirkStaleRing FX-781,
// kDelayQuirkCtorTime FX-782 (the first time equal to the module's 50 ms is a no-op, and the reset does not consume
// pending changes, FX-761). Sixteen: gates open from the reset, rings read as zero after it, the reset applies the
// stored values first (our choice: FX-056 takes precedence over FX-761's "does not consume pending changes"). FX-784
// (lo cut below the 5 Hz floor) is kept.
//
// Real time: prepare() allocates 2 × (trunc(4 s·fs) + 2500) floats (1.43 MB at 44.1 kHz, 1.56 MB at 48 kHz); process()
// is noexcept, exactly 64 frames, no libm per sample. NEON: ring copies, gates, spread and feedback ramps, adds 4
// samples per vector, both filters L/R in two lanes (processScalar() is the bit-equal scalar reference). Budget 9k A9
// cycles per block (tests/bench/fx/delayBus.cpp).
#pragma once

#include "engine/EngineApi.h"
#include "engine/fx/EffectBase.h"
#include "engine/fx/dsp/delay_kernels.h"

#include <cstddef>
#include <cstdint>

namespace engine::fx
{
class SendDelay final : public EffectBase
{
public:
    enum Option : int
    {
        kOptSync = 0 // options[0] "fx.delayBus.sync"
    };

    void prepare(double sampleRate, int maxBlock) override;
    void reset() noexcept override; // FX-761 (+ FX-780/781/782 per profile)
    void setParam(int index, float value01) noexcept override;
    void setOption(int which, int value) noexcept override; // kOptSync, kClearState
    void setQuirkProfile(std::uint32_t mask) noexcept override { quirks_ = mask; }
    void process(float* left, float* right, const ProcessContext&) noexcept override;

    void processScalar(float* left, float* right, float bpm) noexcept;
#if APP_FX_HAVE_NEON
    void processNeon(float* left, float* right, float bpm) noexcept;
#endif

    int delaySamples() const noexcept { return modL_.delaySamples(); }
    float delayMs() const noexcept { return modL_.ms(); }
    double hiCutHz() const noexcept { return hiCut_.cutoff(); }
    double loCutHz() const noexcept { return loCut_.cutoff(); }
    std::size_t bytes() const noexcept { return bytes_; }

    static float freeMs(float v) noexcept;
    static float syncMs(float v, float bpm) noexcept;
    static std::uint32_t hiCutStepOf(float v) noexcept;
    static std::uint32_t loCutStepOf(float v) noexcept;

private:
    void init() noexcept;
    void applyParams() noexcept; // 0x104e98 (before the core)
    void applyBase() noexcept;   // primary slot 8 0x104ce4
    template <bool Neon> void run(float* left, float* right, float bpm) noexcept;

    double fs_ = 44100.0;
    dsp::delay::Param p_[6];
    dsp::delay::Toggle sync_;
    dsp::delay::Param bpm_;
    float baseTL_ = 1.0f, baseTR_ = 1.0f, baseGL_ = 0.0f, baseGR_ = 0.0f; // DelayEchoBase's own parameters
    int syncV_ = 0;
    float bpmV_ = 120.0f, timeV_ = 0.5f;
    bool dirty_ = false;
    dsp::delay::Module<1> modL_, modR_;
    dsp::SmoothFloat fbL_, fbR_, direct_, cross_;
    dsp::delay::SteppedIir1 hiCut_, loCut_;
    std::uint32_t quirks_ = 0;
    bool warmUp_ = false;
    std::size_t bytes_ = 0;
    alignas(16) float s0_[kBlockFrames]{}, s1_[kBlockFrames]{}, s2_[kBlockFrames]{}, s3_[kBlockFrames]{},
        s4_[kBlockFrames]{}, s5_[kBlockFrames]{};
};
} // namespace engine::fx
