// Phaser (spec 02-features/effects/phaser.md FX-320…325, FX-360…362, quirk FX-380; research docs/effects/phaser.md
// F045–F050, CC BY 4.0). Operation for operation as the MIT model tools/fx/phaser.py (class Phaser) with the shared
// ModOscillator as its triangle LFO (phaser.py's LFO class is the same oscillator, waveform 1), so tests/golden/phaser
// and phaser_48k match it through the shell (FxShell: Open gate, 10 Hz wet high-pass, crossfade mix 1 − v / v).
//
//   LFO      triangle, amplitude 0.5, offset 0.5 (0…1), smoothing off; R leads by Phase·0.5 cycle (FX-320)
//   corners  hi = min(tan(π·f/fs), tan(π·20 kHz/fs)); f_lo = f − (1 − (1 − Width)²)·(f − 100);
//            lo = max(tan(π·f_lo/fs), tan(π·100/fs)); f = 300·60^(rint(256v)/256)                          (FX-321)
//   sweep    per block from the LAST LFO sample u: w = lo + (hi − lo)·u³, a_target = (w − 1)/(w + 1); a moves
//            linearly per sample over the block, or over 192 samples after a Rate/Sync/tempo/Phase/Frequency/Width
//            change (FX-322)
//   kernel   per sample and channel: s = (1 − p)·y + p·s (p = exp(−2π·10 kHz/fs)); x = in + fb·s; six all-passes
//            v = x − a·z, x = a·v + z, z = v; g = 0.7 − norm(a)·0.2; out = (in + x)·g; y = out             (FX-323)
//   feedback target 0.99·v with its own 192-sample ramp (FX-324)
//
// Knobs k: 0 rate (free 0.01·rint(1 + 999v) Hz, period 1000/Hz ms; SYNC 64…1/16 beats), 1 width, 2 phase,
// 3 frequency, 4 feedback, 5 mix (the shell's). Option 0 = SYNC ("fx.phaser.sync"); the tempo comes from the
// ProcessContext every block (the reference's BPM index 7). Change detection is the reference's per-parameter
// LinearParameter test with the constructor's "last" values (F026) and its short-circuit order (Rate || BPM || Sync,
// then Frequency || Width) — every value still applies at activation (reset() reads the current values), so FX-056
// needs no quirk bit here. retrigger() restarts the LFO at its peak (resetPhase 0.25) at the next block (FX-360);
// reset() = FX-361; kClearState rebuilds the effect as constructed.
//
// Maths (FX-054): the corner tangents and the frequency law use std:: maths in double rounded to
// float by default (goldens 1e-4); setOption(kOptParityMaths, 1) switches to the fdlibm tanf / powf ports (the
// reference's glibc 2.13 libm; goldens 1e-5). The feedback pole is std::exp in double (correctly rounded, as crmath).
//
// Quirk profile (FX-052; the Reference profile sets every bit, read at the next reset()):
//   kPhaserQuirkPeriod44k   FX-380 (= FX-1380): the LFO period of setFrequency/setPeriodMs from a literal 44,100 Hz
//                           (the initialisation uses fs). Sixteen: the real fs. Identical at 44.1 kHz.
//
// NEON: L/R in the two lanes of a float32x2_t through the whole per-sample chain (feedback low-pass, six
// all-passes, gain), the reference's own kernel layout (0x10cf40, NEON, 2 lanes); the scalar reference does the same
// operations per channel. Budget 14k cycles / block on the A9 (tests/bench/fx/phaser.cpp).
#pragma once

#include "engine/EngineApi.h"
#include "engine/fx/EffectBase.h"
#include "engine/fx/dsp/chorus_params.h"
#include "engine/fx/effects/Oscillator.h"

#include <cstdint>
#include <string_view>

namespace engine::fx
{
constexpr std::uint32_t kPhaserQuirkPeriod44k = 1u << 0; // FX-380

inline constexpr std::string_view kPhaserOptions[] = {"fx.phaser.sync"};

class Phaser final : public EffectBase
{
public:
    enum Option : int
    {
        kOptSync = 0,
        kOptParityMaths = 90, // test hook (FX-054): 1 = the fdlibm tanf / powf ports
    };
    static constexpr int kStages = 6;

    void prepare(double sampleRate, int maxBlock) override;
    void reset() noexcept override; // FX-361
    void setParam(int index, float value01) noexcept override;
    void setOption(int which, int value) noexcept override; // kOptSync, kOptParityMaths, kRetrigger, kClearState
    void process(float* left, float* right, const ProcessContext&) noexcept override;
    void retrigger() noexcept override { retrig_ = true; } // FX-360
    void setQuirkProfile(std::uint32_t mask) noexcept override { quirks_ = mask; }

    void useScalarKernels(bool on) noexcept { scalar_ = on; } // test hook (neon_vs_scalar_phaser)
    float coefficient(int ch) const noexcept { return a_[ch]; }
    float coefficientTarget(int ch) const noexcept { return aT_[ch]; }
    float feedbackNow() const noexcept { return fb_; }
    float periodMs() const noexcept { return periodMs_; }
    float lowCorner() const noexcept { return loT_; }  // tan domain
    float highCorner() const noexcept { return hiT_; } // tan domain
    float coefficientLow() const noexcept { return aLo_; }
    float coefficientHigh() const noexcept { return aHi_; }
    const ModOscillator& oscillator() const noexcept { return lfo_; }
    const float* lfoL() const noexcept { return lL_; } // the last block's LFO values (0…1)
    const float* lfoR() const noexcept { return lR_; }

    // The frequency law of k3 (FX-321): 300·60^(rint(256v)/256) Hz.
    static float frequencyHz(float v, bool parity) noexcept;

private:
    void build() noexcept; // the reference constructor 0x10d654, then the stored values
    float tanOf(float x) const noexcept;
    void initLfo(float hz) noexcept;
    void updateRate(bool apply) noexcept;
    void updateRange(bool setCount) noexcept;
    float coefOf(float u) const noexcept; // a = (w − 1)/(w + 1), w = lo + u³·(hi − lo)
    void kernelScalar(float* l, float* r, float dfb, const float* da) noexcept;
    void kernelNeon(float* l, float* r, float dfb, const float* da) noexcept;

    double fs_ = 44100.0;
    float fsf_ = 44100.0f;
    std::uint32_t quirks_ = 0;
    bool parity_ = false, retrig_ = false, fresh_ = true, scalar_ = false;
    int rc_ = 3 * kBlockFrames, cntA_ = -1, cntF_ = -1;
    float tLo_ = 0.0f, tHi_ = 0.0f, aLo_ = 0.0f, aHi_ = 0.0f, gHi_ = 0.5f, gLo_ = 0.7f;
    float pole_ = 0.0f, pole1_ = 0.0f;
    float loT_ = 0.0f, hiT_ = 0.0f;
    float fb_ = 0.0f, fbT_ = 0.0f, periodMs_ = 0.0f;
    float y_[2]{}, s_[2]{}, a_[2]{}, aT_[2]{};
    float z_[kStages][2]{};
    dsp::mod::LinParam p_[6], pBpm_;
    dsp::mod::Toggle pSync_;
    ModOscillator lfo_;
    float stored_[6]{};
    int storedSync_ = 0;
    std::uint32_t storedMask_ = 0;
    alignas(16) float lL_[kBlockFrames]{};
    alignas(16) float lR_[kBlockFrames]{};
};
} // namespace engine::fx
