// Reverb ("reverb"): a stereo feedback-delay-network reverb for the track inserts (FX1/FX2), the Send FX and the
// Master FX. The app's own design (no reference parity): an 8-line FDN in the late field, input diffusion before it.
//
//   in × g_in(time) → pre-delay (0.1–100 ms, rate-limited glide, linear read while gliding)
//     → one-pole HPF 80 Hz → one-pole LPF 12 kHz (the bandwidth of the reverb input)
//     → 4 series all-passes per channel (different L/R lengths, g 0.75, 0.75, 0.625, 0.625: input diffusion)
//     → Fdn8 (dsp/reverb_kernels.h): 8 lines, a one-pole absorbent filter per line (exact T60 at DC, T60/3 at the
//       damping frequency, at most −3 dB per pass there), the 8-point Walsh–Hadamard feedback matrix, left input into
//       lines 1–3, right into 5–7; lines 1, 3, 5, 7 slowly modulated (±0.25 ms, 0.5–1.1 Hz) against metallic ringing
//     → wet = g_out·late + g_early·diffused input → width (mid/side, Spread)
//   the shell (descriptor): gate mode 1, no wet high-pass, the reverb mix law (wet × 0.70795).
//
// Knobs k (the parameter ids and texts of the earlier reverb, so projects keep their settings):
//   0 pre-delay   ms = 100v, at least 0.1
//   1 room        room = trunc(1 + 99v) ∈ 1…100 → the line lengths: base × 0.2·5^((room − 1)/99) (5.9–66 ms lines);
//                 a change glides every line linearly to its new length (at most 3 % pitch while gliding)
//   2 time        T60 by the decay curve (log T60 linear in v between the anchors, 0.10 s … 100.6 s); g_in 0.2 → 0.1
//                 above v = 0.5137255 so very long decays do not build up
//   3 damping     step = trunc(1 + 99(1 − v)) → fd = 2000·9^((step − 1)/99) Hz, at most min(18 kHz, 0.4·fs)
//   4 spread      stereo width 1 + v (mid/side: 1 = the network's own image, 2 = the side doubled)
//   5 mix         the shell's
//
// CPU (the point of the design: up to 16 track reverbs plus the Send and the Master):
//   - all eight lines are longer than a block, so every ring is read and written as whole blocks (memcpy) and the
//     diffusers run as vector operations; only the matrix runs per sample (NEON: 4 samples × 8 lines transposed into
//     two float32x4 per sample). Steady state ≈ 1.3 × the Distortion insert (instructions executed, NEON build);
//     libm (exp, cos, sqrt) only when Room, Time or Damping change.
//   - sleep: once the input has been silent long enough for everything to have entered the network and the wet
//     output has decayed below −120 dBFS, the reverb clears its rings (O(1)) and returns silence without running;
//     the first non-silent input block wakes it. A reverb on a silent track costs a peak scan per block.
// Memory: pre-delay 2 × 100 ms, the diffusers ≈ 58 ms, the lines at room 100 ≈ 376 ms (+ modulation) — ≈ 117 KB per
// instance at 44.1 kHz, ≈ 128 KB at 48 kHz (34 slots ≈ 4.2 MiB). prepare() allocates; process() is noexcept,
// allocation-free, exactly 64 frames.
#pragma once

#include "engine/EngineApi.h"
#include "engine/fx/EffectBase.h"
#include "engine/fx/dsp/reverb_kernels.h"

#include <cstddef>
#include <cstdint>

namespace engine::fx
{
class Reverb final : public EffectBase
{
public:
    static constexpr int kLines = dsp::reverb::Fdn8::kLines;
    static constexpr int kDiffusers = 4;
    static constexpr float kDampRatio = 3.0f;     // T60 at the damping frequency = T60 / 3
    static constexpr float kSleepLevel = 1.0e-6f; // −120 dBFS: the tail is gone
    static constexpr float kWakeLevel = 1.0e-7f;  // −140 dBFS: the input is silent below this

    void prepare(double sampleRate, int maxBlock) override;
    void reset() noexcept override;                            // FX-862: the stored values at once, rings cleared
    void setParam(int index, float value01) noexcept override; // k0 pre-delay, k1 room, k2 time, k3 damping, k4 spread
    void setOption(int which, int value) noexcept override;    // kClearState
    void process(float* l, float* r, const ProcessContext&) noexcept override;

    void processScalar(float* l, float* r) noexcept;
#if APP_FX_HAVE_NEON
    void processNeon(float* l, float* r) noexcept;
#endif

    // The laws (also used by the tests and the panel formatter checks).
    static float t60(float v) noexcept;        // seconds
    static float roomScale(int room) noexcept; // line-length factor, 0.2 … 1
    static float inputGain(float v) noexcept;  // g_in(time)
    static float dampingHz(float v, double fs) noexcept;
    static float dampingMaxHz(double fs) noexcept; // 18 kHz at 44.1 kHz, min(18 kHz, 0.4·fs) elsewhere
    static float baseDelayMs(int line) noexcept;   // the line lengths at room 100

    // Introspection (tests).
    bool sleeping() const noexcept { return sleeping_; }
    int room() const noexcept { return room_; }
    float lineDelay(int i) const noexcept { return dCur_[i]; }  // samples, without the modulation
    float lineTarget(int i) const noexcept { return dTgt_[i]; } // samples
    float preDelaySamples() const noexcept { return pdCur_; }
    const dsp::reverb::Fdn8& network() const noexcept { return fdn_; }
    std::size_t bytes() const noexcept { return bytes_; }

private:
    void targets(bool snap) noexcept; // knob values → delay and coefficient targets
    void coefficients(bool snap) noexcept;
    void clearState() noexcept;
    template <bool Neon> void run(float* l, float* r) noexcept;

    double fs_ = 44100.0;
    float spm_ = 44.1f; // samples per ms
    float p_[6]{0.0f, 49.0f / 99.0f, 49.0f / 99.0f, 0.0f, 0.0f, 1.0f};
    float lastP_[5]{-1.0f, -1.0f, -1.0f, -1.0f, -1.0f};
    int room_ = 50;

    // pre-delay
    dsp::reverb::Ring pdL_, pdR_;
    float pdCur_ = 4.0f, pdTgt_ = 4.0f, pdStart_ = 4.0f, pdProg_ = 1.0f, pdStep_ = 1.0f;
    // input filters (one-pole HPF / LPF, per channel)
    float hpA_ = 0.0f, lpC_ = 1.0f, hpX_[2]{}, hpY_[2]{}, lpY_[2]{};
    // diffusion
    dsp::reverb::AllPass apL_[kDiffusers], apR_[kDiffusers];
    // late field
    dsp::reverb::Fdn8 fdn_;
    float dCur_[kLines]{}, dTgt_[kLines]{}, dStart_[kLines]{}, dMax_[kLines]{};
    float glide_ = 1.0f, glideStep_ = 1.0f;
    float aTgt_[kLines]{}, bTgt_[kLines]{}, aCur_[kLines]{}, bCur_[kLines]{};
    bool coefMoving_ = false;
    dsp::reverb::BlockSine lfo_[kLines / 2];
    float mod_[kLines]{}, depth_ = 0.0f;
    // output
    float inGain_ = 0.2f, outGain_ = 1.0f, earlyGain_ = 0.0f, width_ = 1.0f;
    // sleep
    int quiet_ = 0, quietNeeded_ = 0;
    bool sleeping_ = false;
    std::size_t bytes_ = 0;
};
} // namespace engine::fx
