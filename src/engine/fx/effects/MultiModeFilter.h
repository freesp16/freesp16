// Multi-mode filter ("filter"; spec 02-features/effects/multimode-filter.md FX-520…524, FX-560/561, FX-580…582; research
// docs/effects/multimode-filter.md, CC BY 4.0). Operation for operation equal to the MIT research model tools/fx/mmf.py
// (pinned commit 8901c97) in the Reference quirk profile; its goldens are tests/golden/filter{,_48k}.
//
//   per 64-frame block:  apply changed parameters (CUTOFF, Q, GAIN, GLIDE, then TYPE + SLOPE → filter kind)
//                        → filter container: cutoff/Q glide (one multiplicative step per 44.1 samples, accumulated),
//                          kind 0–2 one LP/HP/BP biquad at Q, kind 3–5 two cascaded biquads at √Q, kind 6 none; its
//                          132-sample gate for kind switches and gated jumps (Glide 0 and > G·152/1000 steps)
//                        → output GainRamp (132-sample linear ramp to 10^(dB/20))
//   the shell (descriptor): crossfade dry 1 − v / wet v (k5) and the 10 Hz wet high-pass (FX-524).
//
// Parameters (stored step fractions v, FX-060): k0 cutoff fc = f32(f32(pow(1000, v))·20), k1 gain dB = 45v − 30,
// k2 Q = 0.2 + 9.8v, k3 glide G = 64·(1 + 8v), k4 unused, k5 mix (the shell's). Options (descriptor options[i] → i):
// 0 = type1 FilterType {LowPass, HighPass, BandPass, Off}, 1 = type2 Slope {12 dB, 24 dB} (the clean enum, FX-582;
// internally FILTERTYPE 0/1/2/6 and FILTERDB 1/2).
//
// Quirk profile (FX-052/053, effect-local bits; the Reference profile sets every bit, Sixteen none):
//   kQuirkStartupGate  FX-580: the filter gate opens over 132 samples on the very first block. Sixteen: it starts open.
//   kQuirkChangeTrap   FX-581 / FX-056: the reference's constructor "last" values (CUTOFF/GAIN/Q 0.5, FILTERTYPE 6, …)
//                      swallow a first value equal to them (Off set before the first block keeps an LP running).
//                      Sixteen: the first value of every parameter is applied.
// Both only matter for a freshly built (or kClearState-cleared) instance; setQuirkProfile() on such an instance
// re-arms them (the shell sends the profile at every activation, before the parameters, FX-053).
//
// Real time: prepare() allocates nothing (all state is inline, ≈ 1.6 KB per instance); process() is noexcept, exactly 64 frames
// (FX-561), no libm per sample (tan/pow only at glide steps and parameter changes, ≤ 2 per block). processScalar() is the
// scalar reference of process() (NEON: L/R in two lanes, gains 4-wide), bit-equal on normal input.
#pragma once

#include "engine/EngineApi.h"
#include "engine/fx/EffectBase.h"
#include "engine/fx/dsp/filter_blocks.h"

#include <cstdint>

namespace engine::fx::effects
{
class MultiModeFilter final : public EffectBase
{
public:
    enum Option : int
    {
        kOptType = 0, // options[0] "fx.filter.type1"
        kOptSlope = 1 // options[1] "fx.filter.type2"
    };
    enum Type : int
    {
        kLowPass = 0,
        kHighPass = 1,
        kBandPass = 2,
        kOff = 3
    };
    enum Slope : int
    {
        k12dB = 0,
        k24dB = 1
    };
    static constexpr std::uint32_t kQuirkStartupGate = 1u << 0; // FX-580
    static constexpr std::uint32_t kQuirkChangeTrap = 1u << 1;  // FX-581 (FX-056)
    static constexpr int kKindOff = 6;

    // Insert / Master / Send defaults (identical, F175), step fractions; options type LowPass, slope 12 dB.
    static constexpr float kDefaults[6] = {0.566f, 2.0f / 3.0f, 5.0f / 98.0f, 0.0f, 0.0f, 1.0f};

    void prepare(double sampleRate, int maxBlock) override;
    void reset() noexcept override; // FX-560: apply pending changes, zero the current kind's taps only
    void setParam(int index, float value01) noexcept override;
    void setOption(int which, int value) noexcept override; // kOptType, kOptSlope, kClearState
    void process(float* left, float* right, const ProcessContext&) noexcept override;
    void processScalar(float* left, float* right, const ProcessContext&) noexcept;
    void setQuirkProfile(std::uint32_t mask) noexcept override;

    // Introspection for the tests (message thread / test code only).
    int kind() const noexcept { return cur_; }
    int pendingKind() const noexcept { return pending_; }
    int cutoffSteps() const noexcept { return cutSteps_; }
    int qSteps() const noexcept { return qSteps_; }
    float cutoffHz() const noexcept { return cutC_; }
    float cutoffTarget() const noexcept { return cutT_; }
    float q() const noexcept { return qC_; }
    float gain() const noexcept { return gain_.cur; }
    float glideSteps() const noexcept { return g44_; }
    const dsp::filt::Coefs& coefs(int stage) const noexcept;

    // The parameter laws (float32, as the reference), also used by the tests and the catalogue's formatter checks.
    static float cutoffOf(float v) noexcept; // f32(f32(pow(1000, v)) · 20)
    static float qOf(float v) noexcept;      // f32(0.2 + f32(v · 9.8))
    static float gainDbOf(float v) noexcept; // f32(f32(v · 45) − 30)
    static float gainOf(float v) noexcept;   // f32(pow(10, f32(dB · 0.05)))
    static float glideOf(float v) noexcept;  // G = f32(f32(1 + f32(v · 8)) · 64), 64 when |G − 64| < FLT_EPSILON
    static int kindOf(int refType, int refDb) noexcept; // 0x107c24

private:
    struct Stage // a sub-filter wrapper: kind, one or two biquads
    {
        int kind = 0, count = 0;
        dsp::filt::Kind type = dsp::filt::Kind::LowPass;
        float nyq = 22050.0f;
        double Q = 0.7071067811865476;
        float fc = 11027.5f;
        dsp::filt::Iir f[2]{};
    };

    void clear() noexcept; // the freshly built state (reference ctor), quirk-dependent
    void armQuirks() noexcept;
    void applyParams() noexcept;
    void setGlide(float g) noexcept;
    void setKind(int k) noexcept;
    void rampSetup() noexcept;
    Stage& swapKind() noexcept;
    void configure(Stage& s, float cut, float q, bool flag) noexcept;
    template <bool Neon> void runStage(Stage& s, float* l, float* r, int n) noexcept;
    template <bool Neon> void container(float* l, float* r, int n) noexcept;
    template <bool Neon> void run(float* l, float* r) noexcept;

    double fs_ = 44100.0;
    std::uint32_t quirks_ = 0;
    bool pristine_ = true;
    bool userSet_[8]{};
    dsp::filt::RefParam p_[8]{};

    Stage st_[7]{};
    dsp::filt::RefGate gate_{};
    int pending_ = 0, cur_ = 0;
    float cutT_ = 1000.0f, cutC_ = 1000.0f, cutMult_ = 0.0f;
    int cutSteps_ = 0;
    float qT_ = 0.7f, qC_ = 0.7f, qMult_ = 1.0f;
    int qSteps_ = 0;
    float next_ = 0.0f;
    int maxSteps_ = 0;
    bool flag40_ = false, dirty41_ = false, glide42_ = false;
    float g44_ = 64.0f, up48_ = 1.0f, dn4c_ = 1.0f;
    float qUp_ = 1.0f, qDn_ = 1.0f; // 50^(1/100) and its inverse (the static initialiser's globals)
    dsp::filt::RefGainRamp gain_{};
};
} // namespace engine::fx::effects
