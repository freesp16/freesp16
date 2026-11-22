// Insert Delay ("delay"; spec 02-features/effects/insert-delay.md FX-620…625, FX-660…662, quirks FX-680…683; research
// docs/effects/insert-delay.md F075–F080, CC BY 4.0). Operation for operation as the MIT model tools/fx/insert_delay.py
// (class InsertDelay, pinned commit 8901c97) in the Reference quirk profile, so tests/golden/delay matches it through
// the shell with exact integer delay positions.
//
//   per 64-frame block:  BPM from ProcessContext (the processor's parameter-7 feed, F017) → apply changed parameters
//                        (SYNC, BPM, TIME by change detection → the module's setDelay; FEEDBACK, HI CUT)
//                        → y = gate · ring[w − D]   (integer read, the module's 147-sample SignalGate; a time change
//                                                    fades out, jumps, fades in: FX-622)
//                        → y = HiCut(y)             (stepped first-order bilinear LP, 2–20 kHz, FX-621)
//                        → wet = y;  ring[w] = in + g·y   (feedback after the high-cut, 147-sample ramp, FX-620)
//   the shell (descriptor): gate mode 1 (input fade), no wet high-pass, the piecewise delay mix law of k5 (FX-624/625).
//
// Knobs k: 0 time (free: ms = rint(10 + 990v); SYNC: i = rint(5v) of 1/8 … 2 beats, ms = beats·60000/BPM clamped to
// 10…1000), 1 feedback g = rint(100v)/100, 2 hi cut step = trunc(999v) + 1, 3/4 stored and never read (F075), 5 mix
// (the shell's). Option 0 = SYNC (descriptor options[0] "fx.delay.sync", FX-623).
//
// Quirk profile (FX-052, honoured at the next reset — the shell sends it before every activation, FX-053; the
// Reference profile sets every bit, the Sixteen profile none):
//   kDelayQuirkWarmUp     FX-680: after a reset the module gate stays closed until D samples have been counted, so
//                         input of the first D samples never echoes. Sixteen: the gate is open from the reset.
//   kDelayQuirkStaleRing  FX-681: the reset rewinds the ring indices and keeps the samples (and the filter states,
//                         FX-660/761). Sixteen: the ring reads as zero after a reset (an O(1) high-water clear, no
//                         memset: our choice for FX-681's "spread over blocks") and the hi-cut state is zeroed with it,
//                         so no pre-reset audio comes out; the cutoff keeps its glide position.
//   kDelayQuirkCtorTime   FX-682: the constructor's "last" values (time 0.75 …) swallow an equal first value, so the
//                         module keeps its 50 ms. Sixteen: every reset applies the stored values (FX-056).
// FX-683 (feedback > 1) is unreachable: the shell clamps v to [0, 1] (FX-057).
//
// Real time: prepare() allocates the ring (2 × (trunc(1 s·fs) + 2500) floats ≈ 373 KB at 44.1 kHz, 404 KB at 48 kHz);
// process() is noexcept, exactly 64 frames, no libm per sample (tan only when the hi-cut moves a step). NEON: the ring
// copies, feedback ramp, adds and the gate 4 samples per vector, the hi-cut L/R in two lanes (processScalar() is the
// bit-equal scalar reference). Budget 6k A9 cycles per block (tests/bench/fx/delay.cpp).
#pragma once

#include "engine/EngineApi.h"
#include "engine/fx/EffectBase.h"
#include "engine/fx/dsp/delay_kernels.h"

#include <cstddef>
#include <cstdint>

namespace engine::fx
{
constexpr std::uint32_t kDelayQuirkWarmUp = 1u << 0;    // FX-680 / FX-780
constexpr std::uint32_t kDelayQuirkStaleRing = 1u << 1; // FX-681 / FX-781
constexpr std::uint32_t kDelayQuirkCtorTime = 1u << 2;  // FX-682 / FX-782

class InsertDelay final : public EffectBase
{
public:
    enum Option : int
    {
        kOptSync = 0 // options[0] "fx.delay.sync"
    };

    void prepare(double sampleRate, int maxBlock) override;
    void reset() noexcept override; // FX-660 (+ FX-680/681/682 per profile)
    void setParam(int index, float value01) noexcept override;
    void setOption(int which, int value) noexcept override; // kOptSync, kClearState
    void setQuirkProfile(std::uint32_t mask) noexcept override { quirks_ = mask; }
    void process(float* left, float* right, const ProcessContext&) noexcept override;

    // The two implementations (bit-equal on finite input; process() runs NEON on the device).
    void processScalar(float* left, float* right, float bpm) noexcept;
#if APP_FX_HAVE_NEON
    void processNeon(float* left, float* right, float bpm) noexcept;
#endif

    // Introspection (tests, memory log).
    int delaySamples() const noexcept { return mod_.delaySamples(); }
    float delayMs() const noexcept { return mod_.ms(); }
    float feedbackTarget() const noexcept { return fb_.target(); }
    double hiCutHz() const noexcept { return hiCut_.cutoff(); }
    int hiCutStep() const noexcept { return hiCut_.step(); }
    std::size_t bytes() const noexcept { return bytes_; }

    // The parameter laws (insert_delay.py; also used by the tests and the panel's formatter checks).
    static float freeMs(float v) noexcept;
    static float syncMs(float v, float bpm) noexcept;
    static float feedbackGain(float v) noexcept;
    static std::uint32_t hiCutStepOf(float v) noexcept;

private:
    void init() noexcept; // the constructor state (prepare, kClearState)
    void applyParams() noexcept;
    template <bool Neon> void run(float* left, float* right, float bpm) noexcept;

    double fs_ = 44100.0;
    dsp::delay::Param p_[6];
    dsp::delay::Toggle sync_;
    dsp::delay::Param bpm_;
    int syncV_ = 0;
    float bpmV_ = 120.0f, timeV_ = 0.5f;
    bool dirty_ = false;
    dsp::delay::Module<2> mod_;
    dsp::SmoothFloat fb_;
    dsp::delay::SteppedIir1 hiCut_;
    std::uint32_t quirks_ = 0;
    bool warmUp_ = false; // kDelayQuirkWarmUp as of the last reset
    std::size_t bytes_ = 0;
    alignas(16) float yL_[kBlockFrames]{}, yR_[kBlockFrames]{}, wL_[kBlockFrames]{}, wR_[kBlockFrames]{};
};
} // namespace engine::fx
