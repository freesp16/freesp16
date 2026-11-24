// Vintage Pitch Shifter ("vpitch"): a late-1970s digital harmonizer — 10-bit ramp A/D at a 33.5 kHz frame clock,
// a 1K-word RAM window read by a voltage-controlled clock, a staircase crossfade that hides the read pointer's jumps,
// multiplying D/As and a 2:1 / 1:2 compander around the converters. Ported from a standalone emulation written for
// this project (dsp kernels: engine/fx/dsp/vpitch_kernels.h); nothing is reconstructed from instrument firmware.
//
// Signal flow, per 64-frame block (stereo = two identical units in the two lanes of a NEON float32x2_t):
//   S1  in·kTrim + g·fb        fb = the previous block's output (feedback enters before the compressor, as on the
//                               hardware; one block of loop delay — 1.45 ms — next to a loop of ≥ 15 ms)
//   S2  compressor → AA filter  pre-emphasis, 2:1 about a full-scale sine, 4-pole peaked low-pass at 12 kHz
//   S3  the digital board       event-driven: FREF events sample (Hermite at the instant), convert (bow, acquisition,
//                               gating noise, hard clip = LIMIT, dead LSBs) and write the RAM; FIN events (the pitch
//                               clock, ratio × FREF) read tap A, swap it through the 1K echo region into tap B, step
//                               the crossfade and update the D/As (zero-order hold, polyBLEP at the host rate)
//   S4  reconstruction → expander → output buffer (coupling, slew, knee to the rails)
//   S5  × kMakeup → wet (the shell mixes dry/wet: CrossfadeK5)
//
// Pointer law: the read tap runs at FIN, the write tap at FREF; when the read tap catches up (pitch up) or falls a
// window behind (pitch down) it jumps one window (a SPLICE) and the crossfade starts; 512 read events later the
// crossfade is at its peak, which is when the audible tap (B, tap A 1024 events late) reaches the same jump.
//
// Knobs (descriptor page order = internal index k):
//   k0 PITCH     −12 … +12 semitones                  ratio = 2^((100·st + ct)/1200), clamped to 0.5 … 2
//   k1 FINE      −50 … +50 cents
//   k2 DELAY     0 … 120 ms added read delay           (the hardware's 7.5 ms DELAY steps and 30/60 ms ADD'L DELAY
//                                                      in one knob; a change waits for an idle crossfade and is
//                                                      spliced like a pointer jump, so turning it never clicks)
//   k3 FEEDBACK  −100 … +100 %                         sign = FB phase switch, |g| ≤ 0.92 (the FB LEV trim)
//   k4 AGE       0 … 100 %                             every service trim of a worn unit in one macro: A/D bow and
//                                                      noise, D/A bit-weight errors, dead LSBs from 60 %, VCO
//                                                      instability, master-clock drift, compander mismatch, RA/RB
//                                                      trim error (crossfade thump), crossfade phase misalignment
//   k5 MIX       the shell's (CrossfadeK5)
//
// Gain staging: the converters sit at their own full scale (±1 = code 0/1023). kTrim puts a 0 dBFS drum hit, after
// the compressor's 2:1 and its +10 dB pre-emphasis, just under the A/D clip, and kMakeup = 1/kTrim returns the
// expander's output to the input's level: with MIX 100 %, PITCH 0 and AGE 0 the wet level equals the input level
// (tests/unit/fx/test_fx_vpitch.cpp "gain staging").
//
// Real time: no allocation after prepare() (every buffer is a member, ~60 KB), no libm per sample (log2/exp2 are
// polynomial, floor is a truncation), process() is noexcept and handles exactly 64 frames in place. The control
// work (ratio, AGE → LUTs) runs only when a knob moved. processScalar() and processNeon() are bit-equal on normal
// input. The compander gains (log2 → exp2) are evaluated once per 4 samples and ramped (dsp::vpitch::kGainQuad).
// Budget 40k A9 cycles per block (tests/bench/fx/vpitch.cpp).
#pragma once

#include "engine/EngineApi.h"
#include "engine/fx/EffectBase.h"
#include "engine/fx/dsp/vpitch_kernels.h"

#include <cstdint>

namespace engine::fx
{
class VintagePitch final : public EffectBase
{
public:
    // telemetry(): the effect panel's live values (pitchSplice visual, UID-834)
    enum Telemetry : int
    {
        kTelLimit = 0,  // A/D LIMIT activity: 1 on a clipped block, decaying over ~0.3 s (the panel's LIMIT LED)
        kTelPointer = 1 // the read pointer's distance behind the write pointer, 0 … 1 of the window (block end)
    };
    static constexpr float kTrim = 0.25f;  // −12 dB into the compressor
    static constexpr float kMakeup = 4.0f; // +12 dB after the output buffer
    static constexpr float kFeedbackCeiling = 0.92f;

    void prepare(double sampleRate, int maxBlock) override;
    void reset() noexcept override;
    void setParam(int index, float value01) noexcept override;
    void setOption(int which, int value) noexcept override; // kClearState
    void process(float* left, float* right, const ProcessContext&) noexcept override;
    float telemetry(int which) const noexcept override;

    void processScalar(float* left, float* right) noexcept;
#if APP_FX_HAVE_NEON
    void processNeon(float* left, float* right) noexcept;
#endif

    // The parameter laws (also the catalogue formatters' and the tests').
    static int semitones(float v) noexcept;   // −12 … 12
    static int cents(float v) noexcept;       // −50 … 50
    static int delayMs(float v) noexcept;     // 0 … 120
    static int feedbackPct(float v) noexcept; // −100 … 100
    static int agePct(float v) noexcept;      // 0 … 100
    static float ratioOf(int semitones, int cents) noexcept;

    // Introspection (tests, visual checks).
    float ratio() const noexcept { return ratio_; }
    int presetFrames() const noexcept { return preset_; }
    int pointerDelay() const noexcept { return static_cast<int>(w_ - readPos_) - preset_; }
    std::uint32_t splices() const noexcept { return splices_; }
    std::uint32_t limitHits() const noexcept { return limitHits_; }
    int deadLsbs() const noexcept { return deadLsbs_; }

private:
    template <bool Neon> struct Chain // the per-lane state (L, R)
    {
        dsp::vpitch::Comp2<Neon> comp;
        dsp::vpitch::Svf2<Neon> aa1, aa2, recon;
        dsp::vpitch::Expand2<Neon> expand;
        dsp::vpitch::Out2<Neon> out;
        dsp::vpitch::Blep2<Neon> blep;
        dsp::vpitch::V2<Neon> h0, h1, h2, h3; // the last four anti-aliased host samples (the A/D's S&H input)
        dsp::vpitch::V2<Neon> held;           // the S&H capacitor (incomplete acquisition)
        dsp::vpitch::Rng2<Neon> noise;
        void reset(const dsp::vpitch::CompanderCoefs&) noexcept;
    };

    void init() noexcept;          // the constructor state (prepare, kClearState)
    void applyParams() noexcept;   // knob changes → coefficients (control rate, only on change)
    void setAge(int pct) noexcept; // AGE macro → every trim, the D/A tables
    void control() noexcept;       // per block: VCO, clocks, smoothers, deferred DELAY change
    void clearMemory() noexcept;
    void splice() noexcept;
    void xfadeStep() noexcept;
    template <bool Neon> Chain<Neon>& chain() noexcept;
    template <bool Neon> void run(float* left, float* right) noexcept;
    template <bool Neon> void onFref(Chain<Neon>& c, float t) noexcept;
    template <bool Neon> void onFin(Chain<Neon>& c, float t) noexcept;

    double fs_ = 44100.0;
    float v_[6] = {19.0f / 24.0f, 0.5f, 0.0f, 0.5f, 0.35f, 0.5f}; // stored knob fractions (the catalogue defaults)
    float applied_[6] = {-1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f};

    // control
    float ratio_ = 1.0f, inst_ = 0.0f, clockDetune_ = 0.0f;
    int age_ = -1;
    int presetTarget_ = 0;
    float fbTarget_ = 0.0f, fbCur_ = 0.0f;
    float drift_ = 0.0f, fmPhase_ = 0.0f, driftA_ = 0.0f, driftB_ = 0.0f, fmInc_ = 0.0f;
    float limitHold_ = 0.0f, limitDecay_ = 0.99f;
    dsp::vpitch::Rng ctlRng_, jitRng_;

    // coefficients
    dsp::vpitch::SvfCoefs aa1_, aa2_, recon_;
    dsp::vpitch::CompanderCoefs comp_;
    float outCouplingR_ = 0.999f, outMaxStep_ = 1.0f;
    float acq_ = 0.97f, bow_ = 0.01f, noiseLsb_ = 0.6f, adcGain_ = 1.0f, adcOffset_ = 0.0f;
    std::int32_t codeMask_ = ~0;
    int deadLsbs_ = 0;
    float refBGain_ = 1.0f;
    int xfSteps_ = 128, xfStart_ = 512;
    float xfInvSteps_ = 1.0f / 128.0f;

    // the digital board
    float incRef_ = 0.76f, invIncRef_ = 1.3f, incIn_ = 0.76f, invIncIn_ = 1.3f, phRef_ = 0.0f, phIn_ = 0.0f;
    float finGrid_ = 291.0f, invFinGrid_ = 1.0f / 291.0f, jitter_ = 0.0f;
    std::uint32_t w_ = 0, readPos_ = 0, e_ = 0, splices_ = 0, limitHits_ = 0, blockLimits_ = 0;
    int preset_ = 0;
    int xfCountdown_ = -1, xfCount_ = -1;
    float refA_ = 0.0f;
    float lutA_[1024]{}, lutB_[1024]{}, errA_[1024]{}, errB_[1024]{};
    std::int16_t ram_[dsp::vpitch::kRamWords][2]{};
    std::int16_t echo_[dsp::vpitch::kEchoWords][2]{};

    Chain<false> scalar_;
#if APP_FX_HAVE_NEON
    Chain<true> neon_;
#endif

    alignas(16) float xL_[kBlockFrames]{}, xR_[kBlockFrames]{};   // S1/S2 work
    alignas(16) float dL_[kBlockFrames]{}, dR_[kBlockFrames]{};   // S3 D/A output
    alignas(16) float fbL_[kBlockFrames]{}, fbR_[kBlockFrames]{}; // S4 output = next block's feedback
};
} // namespace engine::fx
