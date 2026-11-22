// Chorus (spec 02-features/effects/chorus.md FX-100, FX-120…124, FX-160…163, quirks FX-180…182; research
// docs/effects/chorus.md F054–F062, CC BY 4.0). Operation for operation as the MIT model tools/fx/chorus.py (classes
// Kernel = the reference's c_chorus, Chorus) in the Reference quirk profile, so tests/golden/chorus matches it through
// the shell (FxShell: Open gate, 10 Hz wet high-pass, out = in + Mix·wet).
//
//   LFO     the shared ModOscillator (engine/fx/effects/Oscillator.h): sine, 50 Hz smoother, one call per block into a
//           64-entry buffer per channel: delay d = offset + amp·sin [samples], R leads by Phase (FX-121)
//   depth   t = d·s/100 ms, amp = f32(f32(f32(t·fs)·0.001)·0.5), offset = amp + 0.2                     (FX-120)
//   ring    int(fs/10) + 64 per channel; per sample ring[w] = x, read at pos = w − d (+ length if negative):
//           y = f32(fr·ring[i0 + 1]) + f32((1 − fr)·ring[i0]), no feedback                               (FX-122)
//   gain    g_out (FX-123): the Reference start-up mute (FX-180), else 1 (the multiply is skipped: 1·y = y)
//   rate    k0 free 0.1·rint(1 + 99v) Hz or SYNC (division beats, the tempo of ProcessContext every block),
//           clamped to 0.00104167…64 Hz, applied at the next block; k1/k2/k3 apply at once (FX-160)
//
// Knobs k: 0 rate, 1 width (depth), 2 phase, 3 delay (depth scale), 4 unused, 5 mix (the shell's). Option 0 = SYNC
// ("fx.chorus.sync": the position in the descriptor's option list). retrigger() / setOption(kRetrigger)
// restart the LFO at phase 0 at the next block (FX-162); reset() (activation, FX-161) zeroes the rings and the LFO
// buffers and keeps the write index, the LFO phase and g_out; kClearState rebuilds the kernel as constructed.
//
// Quirk profile (FX-052; the Reference profile sets every bit, read at the next reset()):
//   kChorusQuirkStartMute   FX-180: g_out = 0 until the ring has wrapped once, then the 0.0033·fs ramp. Sixteen: 1.
//   kChorusQuirkPhase380    FX-181: phase degrees / 380 (max 170.5°). Sixteen: / 360.
//   kChorusQuirkFixed44k    FX-182 (+ FX-1380): kernel and oscillator built at 44,100 Hz, the oscillator period from
//                           a literal 44,100. Sixteen: the real fs.
//   kChorusQuirkChangeTrap  (our bit; research F026): the constructor's RATE "last" value 0.5 makes a first RATE of 0.5
//                           a no-op (the kernel's 0.1 Hz stays). Sixteen: the first block always applies the rate.
//
// NEON: the ring writes stay scalar; per 4 samples the 4 read positions, the integer
// taps and the fractions are vectors, the 8 taps are gathered with lane loads and interpolated in vectors (the
// scalar reference keeps the model's per-sample branches; same results: every lane step is the same IEEE operation —
// the 4-sample write-ahead is invisible because a read never reaches the next write position, d >= 0.2 after the
// first sample and > ulp(w) on the first). Budget 10k cycles / 64-frame block on the A9 (tests/bench/fx/chorus.cpp).
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
constexpr std::uint32_t kChorusQuirkStartMute = 1u << 0;  // FX-180
constexpr std::uint32_t kChorusQuirkPhase380 = 1u << 1;   // FX-181
constexpr std::uint32_t kChorusQuirkFixed44k = 1u << 2;   // FX-182 / FX-1380
constexpr std::uint32_t kChorusQuirkChangeTrap = 1u << 3; // research F026 (our quirk bit)

inline constexpr std::string_view kChorusOptions[] = {"fx.chorus.sync"};

class Chorus final : public EffectBase
{
public:
    enum Option : int
    {
        kOptSync = 0
    };

    void prepare(double sampleRate, int maxBlock) override; // rings int(fs/10) + 64 per channel (and at 44.1 kHz)
    void reset() noexcept override;                          // FX-161
    void setParam(int index, float value01) noexcept override;
    void setOption(int which, int value) noexcept override; // kOptSync, kRetrigger, kClearState
    void process(float* left, float* right, const ProcessContext&) noexcept override;
    void retrigger() noexcept override { retrig_ = true; } // FX-162
    void setQuirkProfile(std::uint32_t mask) noexcept override { quirks_ = mask; }

    void useScalarKernels(bool on) noexcept { scalar_ = on; } // test hook (neon_vs_scalar_chorus)
    float outputGain() const noexcept { return g_; }
    int ringLength() const noexcept { return ln_; }
    int kernelRate() const noexcept { return kfs_; }
    float rateHz() const noexcept { return rate_; }
    const ModOscillator& oscillator() const noexcept { return osc_; }
    const float* delayL() const noexcept { return dL_; } // the last block's LFO delays [samples]
    const float* delayR() const noexcept { return dR_; }

private:
    void build() noexcept; // the reference constructor (kernel 0xff008 + Chorus fields), then the stored values
    int kernelRateFor(std::uint32_t quirks) const noexcept;
    void apply(int k, float v) noexcept;
    void setRate(float hz) noexcept;
    void applyAmp() noexcept;
    void setDepth(float v) noexcept;
    void setScale(float v) noexcept;
    void setPhase(float v) noexcept;
    void zeroRings() noexcept;
    void delayScalar(float* l, float* r) noexcept;
    void delayNeon(float* l, float* r) noexcept;
    void gainStage(float* l, float* r) noexcept;

    double fs_ = 44100.0;
    std::uint32_t quirks_ = 0, built_ = 0; // the profile at the last build()
    int kfs_ = 44100, ln_ = 0, w_ = 0;
    std::vector<float> ring_; // two rings of capacity cap_ (L at 0, R at cap_), allocated in prepare()
    int cap_ = 0;
    ModOscillator osc_;
    float rate_ = 0.1f, depth_ = 100.0f, phase_ = 0.0f, scale_ = 1.0f;
    float g_ = 0.0f, target_ = 0.0f, step_ = 0.0f, coef_ = 0.0f, tol_ = 0.0f;
    bool full_ = false, resetFlag_ = true, retrig_ = false, fresh_ = true, scalar_ = false;
    dsp::mod::LinParam pRate_, pBpm_;
    dsp::mod::Toggle pSync_;
    float rateV_ = 0.5f, bpmV_ = 120.0f;
    int syncV_ = 0;
    bool dirty_ = false;
    float stored_[6]{};
    int storedSync_ = 0;
    std::uint32_t storedMask_ = 0; // bit k: knob k was set; bit 6: SYNC was set
    alignas(16) float dL_[kBlockFrames]{};
    alignas(16) float dR_[kBlockFrames]{};
};
} // namespace engine::fx
