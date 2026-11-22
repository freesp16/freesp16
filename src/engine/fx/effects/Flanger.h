// Flanger (spec 02-features/effects/flanger.md FX-200, FX-220…224, FX-260…263, quirks FX-280…282; research
// docs/effects/flanger.md F063–F068, CC BY 4.0). Operation for operation as the MIT model tools/fx/flanger.py (classes
// Kernel = the reference's c_flanger, Flanger) in the Reference quirk profile, so tests/golden/flanger matches it
// through the shell (FxShell: Open gate, 10 Hz wet high-pass, out = in + Mix·wet).
//
//   LFO     the shared ModOscillator: sine, 50 Hz smoother; d = offset + amp·sin [samples] per channel, R leads by
//           Phase; amp = f32(f32(f32(fs·t)·0.001)·0.5), t = d·s/100 ms, offset = amp + 0.2               (FX-220/221)
//   ring    7·fs/1000 + 64 per channel; per sample, in this order (FX-222):
//             r = w − trunc(d) (+ length if negative); ring[w] = x + f32(y_prev·fb); w advances;
//             fr = d − trunc(d); y = f32(f32((1 − fr)·ring[r]) + f32(fr·ring[r − 1]))  (r − 1 wraps); y_prev = y
//   gain    g(d) = f32(f32(1 + f32(d·0.013154167)) − 0.6314) for d <= 48, else 1 (strict knee), after the loop
//           (FX-223); the pre-gain y feeds back; no limiter (FX-224)
//   rate    k0 free 0.01·rint(1 + 99v) Hz or SYNC (64…1 beats per cycle), clamped to 0.00026042…128 Hz, at the next
//           block; k1…k4 at once
//
// Knobs k: 0 rate, 1 width (depth), 2 phase, 3 delay (depth scale), 4 feedback, 5 mix (the shell's). Option 0 = SYNC
// ("fx.flanger.sync"). retrigger() / setOption(kRetrigger) restart the LFO at phase 0 (FX-262); reset() (activation,
// FX-261) zeroes the rings and buffers and keeps the write index, y_prev and the feedback; no start-up mute (FX-260).
//
// Quirk profile (FX-052; the Reference profile sets every bit, read at the next reset()):
//   kFlangerQuirkFixed44k    FX-280 (+ FX-1380): kernel and oscillator built at 44,100 Hz. Sixteen: the real fs.
//   kFlangerQuirkPhase380    FX-281: phase degrees / 380 (max 170.5°). Sixteen: / 360.
//   kFlangerQuirkChangeTrap  (our bit; F026, FX-056): a first RATE of 0.5 (the constructor's "last") is a no-op.
// FX-282: every field (the LFO counter included) is initialised to 0, the state the model assumes, in both profiles.
//
// NEON: the feedback makes the loop sample-serial, so L and R run in the two lanes of a float32x2_t; the
// integer taps, fractions and the delay gains are computed 4 samples per vector before the loop (the scalar reference
// computes the same values per sample; same IEEE operations). Budget 10k cycles / block (tests/bench/fx/flanger.cpp).
#pragma once

#include "engine/EngineApi.h"
#include "engine/fx/EffectBase.h"
#include "engine/fx/dsp/chorus_params.h"
#include "engine/fx/effects/Oscillator.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace engine::fx
{
constexpr std::uint32_t kFlangerQuirkFixed44k = 1u << 0;  // FX-280 / FX-1380
constexpr std::uint32_t kFlangerQuirkPhase380 = 1u << 1;  // FX-281
constexpr std::uint32_t kFlangerQuirkChangeTrap = 1u << 2; // research F026 (our quirk bit)

inline constexpr std::string_view kFlangerOptions[] = {"fx.flanger.sync"};

class Flanger final : public EffectBase
{
public:
    enum Option : int
    {
        kOptSync = 0
    };

    void prepare(double sampleRate, int maxBlock) override; // rings 7·fs/1000 + 64 per channel (and at 44.1 kHz)
    void reset() noexcept override;                          // FX-261
    void setParam(int index, float value01) noexcept override;
    void setOption(int which, int value) noexcept override; // kOptSync, kRetrigger, kClearState
    void process(float* left, float* right, const ProcessContext&) noexcept override;
    void retrigger() noexcept override { retrig_ = true; } // FX-262
    void setQuirkProfile(std::uint32_t mask) noexcept override { quirks_ = mask; }

    void useScalarKernels(bool on) noexcept { scalar_ = on; } // test hook (neon_vs_scalar_flanger)
    int ringLength() const noexcept { return ln_; }
    int kernelRate() const noexcept { return kfs_; }
    float rateHz() const noexcept { return rate_; }
    float feedback() const noexcept { return fb_; }
    const ModOscillator& oscillator() const noexcept { return osc_; }
    const float* delayL() const noexcept { return dL_; } // the last block's LFO delays [samples]
    const float* delayR() const noexcept { return dR_; }

    static float delayGain(float d) noexcept; // FX-223 (exposed for tests)

private:
    void build() noexcept; // the reference constructor (kernel 0x100234 + Flanger fields), then the stored values
    int kernelRateFor(std::uint32_t quirks) const noexcept;
    void apply(int k, float v) noexcept;
    void setRate(float hz) noexcept;
    void applyAmp() noexcept;
    void setDepth(float v) noexcept;
    void setScale(float v) noexcept;
    void setPhase(float v) noexcept;
    void setFeedback(float v) noexcept;
    void zeroBuffers() noexcept;
    void prepass() noexcept;     // integer taps, fractions, gains of the block (both kernels)
    void loopScalar(float* l, float* r) noexcept;
    void loopNeon(float* l, float* r) noexcept;
    void gainStage(float* l, float* r) noexcept;

    double fs_ = 44100.0;
    std::uint32_t quirks_ = 0;
    int kfs_ = 44100, ln_ = 0, w_ = 0, cap_ = 0;
    std::vector<float> ring_; // two rings of capacity cap_ (L at 0, R at cap_), allocated in prepare()
    ModOscillator osc_;
    float rate_ = 0.1f, depth_ = 100.0f, phase_ = 0.0f, scale_ = 2.0f, fbI_ = 0.0f, fb_ = 0.0f;
    float y_[2]{};
    bool resetFlag_ = true, retrig_ = false, fresh_ = true, scalar_ = false;
    dsp::mod::LinParam pRate_, pBpm_;
    dsp::mod::Toggle pSync_;
    float rateV_ = 0.5f, bpmV_ = 120.0f;
    int syncV_ = 0;
    bool dirty_ = false;
    float stored_[6]{};
    int storedSync_ = 0;
    std::uint32_t storedMask_ = 0;
    alignas(16) float dL_[kBlockFrames]{};
    alignas(16) float dR_[kBlockFrames]{};
    alignas(16) float gL_[kBlockFrames]{};
    alignas(16) float gR_[kBlockFrames]{};
    alignas(16) float frL_[kBlockFrames]{};
    alignas(16) float frR_[kBlockFrames]{};
    alignas(16) std::int32_t iL_[kBlockFrames]{};
    alignas(16) std::int32_t iR_[kBlockFrames]{};
};
} // namespace engine::fx
