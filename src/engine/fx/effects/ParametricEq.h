// 2-band EQ ("eq2"; spec 02-features/effects/parametric-eq.md FX-420…427, FX-460/461, FX-480/481; research
// docs/effects/parametric-eq.md, CC BY 4.0). Operation for operation equal to the MIT research model tools/fx/peq.py
// (pinned commit 8901c97) in the Reference quirk profile; its goldens are tests/golden/eq2{,_48k}.
//
//   per 64-frame block: apply changed parameters (B1F, B1G, B1Q, B2F, B2G, B2Q, TYPE1, TYPE2) → band 1 → band 2
//   one band:  ramp setup → stepped glide (one step per 44.1 samples: frequency ×1000^(1/250), Q ×10^0.02, gain ±0.04 dB)
//              → the current stage's filter → the band's 44-sample GainRamp → the band's 132-sample gate (type
//              switches and gated jumps above 38 frequency / 70 Q / 250 gain steps, FX-425/426)
//   stages:    0 low shelf (+g), 1 low shelf (−g) then ×10^(g/20) = high shelf, 2 peak (+g), 3 LP 12 dB then
//              ×10^(g/20), 4 HP 12 dB then ×10^(g/20) (FX-420); shelf/peak = dsp/eq2_shelf.h, LP/HP = the shared
//              bilinear biquad (FX-424)
//   the shell (descriptor): dry 0, wet 1, no wet high-pass, no tempo (FX-461).
//
// Parameters (stored step fractions v): k0/k3 frequency f = f32(powf(1000, v) · 20), k1/k4 gain dB = 45v − 30, k2/k5
// Q = 0.1 + 9.9v. The frequency law calls the reference's single-precision powf: default builds use std::pow in double
// rounded to float (≤ 6 ulp from it, FX-054: goldens at 1e-4); with parity maths (setOption(kOptParityMaths, 1), a
// test-build hook) the fdlibm powf port of dsp/filter_fdlibm.h (goldens bit-exact). Options (descriptor options[i] → i):
// 0 = type1 band 1 type, 1 = type2 band 2 type, both the app enum in the UI order of FX-069 {HPF 0, LPF 1, PEAK 2,
// HI-SHELF 3, LO-SHELF 4} (FX-070), mapped to the reference's band type (BandType below) as 4 − v; values
// outside 0…4 are clamped (FX-057, so the reference's type 5 null read, FX-481, is unreachable).
//
// Quirk profile (effect-local bits; Reference = every bit, Sixteen = none):
//   kQuirkStartupGate  FX-480: both band gates are built closed and the first block is not equalised at all (through
//                      the shell: a one-time dip to −3.68 dB). Sixteen: the gates start open and the stages run from
//                      block 0 (from block 3 on equal to the reference up to the filters' decaying start transient,
//                      which the reference starts one block later; at the defaults within 1e-5).
//   kQuirkChangeTrap   FX-056 (F026): the constructor "last" values (0.5, type 5) swallow an equal first value.
//                      Sixteen: the first value of every parameter is applied.
// Both act on a freshly built (or kClearState-cleared) instance only.
//
// Real time: no allocation (inline state ≈ 2.8 KB per instance), exactly 64 frames, no libm per sample (tan/pow only
// at glide steps and parameter changes). processScalar() is the scalar reference of process() (NEON: L/R lanes, gains
// 4-wide).
#pragma once

#include "engine/EngineApi.h"
#include "engine/fx/EffectBase.h"
#include "engine/fx/dsp/eq2_shelf.h"
#include "engine/fx/dsp/filter_blocks.h"

#include <cstdint>

namespace engine::fx::effects
{
class ParametricEq final : public EffectBase
{
public:
    enum Option : int
    {
        kOptType1 = 0,        // options[0] "fx.eq2.type1"
        kOptType2 = 1,        // options[1] "fx.eq2.type2"
        kOptParityMaths = 90, // test hook (FX-054): 1 = the frequency law through the fdlibm powf port
    };
    enum AppBandType : int // the option values (catalogue enum order)
    {
        kAppHpf = 0,
        kAppLpf = 1,
        kAppPeak = 2,
        kAppHiShelf = 3,
        kAppLoShelf = 4
    };
    enum BandType : int // the reference's band types (internal)
    {
        kLowShelf = 0,
        kHighShelf = 1,
        kPeak = 2,
        kLowPass = 3,
        kHighPass = 4
    };
    static constexpr std::uint32_t kQuirkStartupGate = 1u << 0; // FX-480
    static constexpr std::uint32_t kQuirkChangeTrap = 1u << 1;  // FX-056 (F026)

    // Insert / Master / Send defaults (F154/F156), by internal index; types: band 1 low shelf, band 2 high shelf.
    static constexpr float kDefaults[6] = {0.566f, 2.0f / 3.0f, 6.0f / 99.0f, 0.566f, 2.0f / 3.0f, 6.0f / 99.0f};

    void prepare(double sampleRate, int maxBlock) override;
    void reset() noexcept override; // FX-460: apply pending changes, zero each band's current stage taps only
    void setParam(int index, float value01) noexcept override;
    void setOption(int which, int value) noexcept override;
    void process(float* left, float* right, const ProcessContext&) noexcept override;
    void processScalar(float* left, float* right, const ProcessContext&) noexcept;
    void setQuirkProfile(std::uint32_t mask) noexcept override;

    // Introspection for the tests.
    struct BandView
    {
        int type, pending, fSteps, qSteps, gSteps;
        float fc, qc, gc, gain;
    };
    BandView band(int b) const noexcept;
    const dsp::filt::Coefs& coefs(int b) const noexcept;

    // Parameter laws (float32, as the reference).
    static float freqOf(float v, bool parityMaths) noexcept; // f32(powf(1000, v) · 20)
    static float gainDbOf(float v) noexcept;                 // f32(f32(v · 45) − 30)
    static float qOf(float v) noexcept;                      // f32(0.1 + f32(v · 9.9))
    static float gainOf(float gdb) noexcept;                 // f32(pow(10, f32(gdb · 0.05)))

private:
    struct Stage // 0x10-byte stage: the shelf/peak filter (types 0–2) or the bilinear biquad (3/4) + the band's gain
    {
        int kind = 0;
        dsp::eq2::Shelf shelf{};     // types 0, 1 (low shelf), 2 (peak)
        dsp::filt::Iir biquad{};     // types 3 (LP), 4 (HP)
        float nyq = 22050.0f;
        double Q = 0.7071067811865476; // the biquad's
        float fc = 11027.5f;
    };
    struct Band
    {
        Stage st[5]{};
        dsp::filt::RefGate gate{};
        dsp::filt::RefGainRamp gain{};
        int pending = 0, cur = 0;
        float ft = 1000.0f, fc = 1000.0f, fmult = 1.0f;
        int fsteps = 0;
        float qt = 0.7f, qc = 0.7f, qmult = 1.0f;
        int qsteps = 0;
        float gt = 0.0f, gc = 0.0f, gstep = 0.04f;
        int gsteps = 0;
        float next = 0.0f;
        int maxsteps = 0;
        bool flag60 = false, flag61 = false, dirty62 = false;
    };

    void clear() noexcept;
    void armQuirks() noexcept;
    void applyParams() noexcept;
    void configure(Band& b, Stage& s, float fc, float qc, float gc, bool flag, bool swap) noexcept;
    void rampSetup(Band& b) noexcept;
    void step(Band& b) noexcept;
    void swapType(Band& b) noexcept;
    template <bool Neon> void runStage(Band& b, Stage& s, float* l, float* r, int n) noexcept;
    template <bool Neon> void runBand(Band& b, float* l, float* r, int n) noexcept;
    template <bool Neon> void run(float* l, float* r) noexcept;

    double fs_ = 44100.0;
    std::uint32_t quirks_ = 0;
    bool pristine_ = true, parityMaths_ = false;
    bool userSet_[8]{};
    dsp::filt::RefParam p_[8]{};
    Band bands_[2]{};
    float mF_ = 1.0f, mFi_ = 1.0f, mQ_ = 1.0f, mQi_ = 1.0f; // the static initialiser's glide multipliers
};
} // namespace engine::fx::effects
