// LoFi (spec 02-features/effects/lofi.md FX-920…926, FX-960/961, quirks FX-980…982; research docs/effects/lofi.md
// F025–F031, F150–F153, CC BY 4.0). Operation for operation as the MIT model tools/fx/lofi.py (class LoFi) in the
// Reference quirk profile, so tests/golden/lofi matches it through the shell.
//
//   Pre:   in → [position gate] → [filter] → [preamp + clip] → [crusher] → [sample-and-hold] → wet
//   Post:  in → [position gate] → [preamp + clip] → [crusher] → [sample-and-hold] → [filter] → wet
//   preamp  y = clip(x, ±1/g)·f32(f32(1 + 0.2g)·0.8333333), g = 10^(42v/20) reached by a 44-sample ramp (FX-920)
//   crusher y = s·(2^−b·trunc(2^b·√|x|))², skipped when off (FX-921)
//   S&H     phase φ, step fs/rate: if φ < 1 { φ += step; held = x }; out = held; φ −= 1 (FX-922)
//   filter  lofi_biquad.h (FX-923) with cutoff ×1000^(1/250) / Q ×50^(1/100) steps every 44.1 samples, gated jumps
//           above 38 steps and type crossfades through a 132-sample gate, priming the new filter (FX-924); position
//           change through a 132-sample position gate (FX-925). The shell does the crossfade mix and the 10 Hz wet HPF.
//
// Knobs k (= page order): 0 preamp, 1 reduction, 2 bit depth, 3 cutoff, 4 resonance, 5 mix (the shell's).
// Options (the descriptor's option index): 0 filter type (0 HP, 1 LP, 2 BP, 3 Off), 1 position (0 Pre, 1 Post).
//
// Quirk profile (FX-052, honoured at the next activation; the Reference profile sets every bit):
//   kLoFiQuirkClosedGates  FX-980: both gates start closed and open over 132 samples on the first block. Sixteen: the
//                          gates are built open and the first parameter set applies at once (no start-up fade).
//   kLoFiQuirkHoldPhase    FX-981: reset sets the hold phase to the step, so the wet path is 0 for ⌊step⌋ samples.
//                          Sixteen: phase 0 (the first input sample is taken at once).
//   kLoFiQuirkChangeTrap   (our bit; research F026): the constructor's "last" values (0.5 for every knob, type 0,
//                          position 2) make a first value equal to them a no-op — e.g. CUT OFF 0.5 at the first
//                          activation keeps the constructor's 1 kHz. Sixteen: the first parameter set always applies
//                          (an option never set means the app default, Off / Post).
// FX-982: a bit-depth index past the table is clamped (unreachable through the shell, FX-057).
//
// NEON: preamp/clip and crusher 4 samples per vector (the square root stays the exact VFP vsqrt, our choice: so
// NEON = scalar bit for bit and the goldens hold in both profiles), biquad L/R in two lanes, gates through the kit's
// vec:: kernels; S&H scalar. Budget 12k cycles / 64-frame block on the A9 (tests/bench/fx/lofi.cpp).
#pragma once

#include "engine/fx/EffectBase.h"
#include "engine/fx/dsp/Ramps.h"
#include "engine/fx/dsp/lofi_biquad.h"

#include <cstdint>

namespace engine::fx
{
constexpr std::uint32_t kLoFiQuirkClosedGates = 1u << 0; // FX-980
constexpr std::uint32_t kLoFiQuirkHoldPhase = 1u << 1;   // FX-981
constexpr std::uint32_t kLoFiQuirkChangeTrap = 1u << 2;  // research F026 (our quirk bit)

class LoFi final : public EffectBase
{
public:
    enum Option : int
    {
        kOptFilterType = 0,
        kOptPosition = 1
    };
    enum FilterType : int
    {
        kHighPass = 0,
        kLowPass = 1,
        kBandPass = 2,
        kFilterOff = 3
    };

    void prepare(double sampleRate, int maxBlock) override;
    void reset() noexcept override; // FX-960 (+ FX-981 per profile)
    void setParam(int index, float value01) noexcept override;
    void setOption(int which, int value) noexcept override; // kOptFilterType, kOptPosition, kClearState
    void setQuirkProfile(std::uint32_t mask) noexcept override;
    void process(float* left, float* right, const ProcessContext&) noexcept override;

    // Test hook (neon_vs_scalar_lofi): run the scalar reference kernels instead of the NEON ones.
    void useScalarKernels(bool on) noexcept { scalar_ = on; }
    // Introspection for tests.
    float holdStep() const noexcept { return shStep_; }
    int filterType() const noexcept { return filt_.cur; }
    int position() const noexcept { return posCur_; }

    class Gate // lofi.py SignalGate on the kit's SmoothFloat (the container needs its ramp state)
    {
    public:
        void prepare(double fs, int n) noexcept { ramp_.prepareSamples(fs, n); }
        void setNow(float v) noexcept { ramp_.snap(v); }
        void open() noexcept { ramp_.setTarget(1.0f); }
        void close() noexcept { ramp_.setTarget(0.0f); }
        bool ramping() const noexcept { return ramp_.ramping(); }
        float target() const noexcept { return ramp_.target(); }
        float current() const noexcept { return ramp_.current(); }
        int remaining() const noexcept { return ramp_.remaining(); }
        bool fadingOut() const noexcept;
        bool settledClosed() const noexcept;
        void apply(float* l, float* r, int n) noexcept;

    private:
        dsp::SmoothFloat ramp_;
    };

private:
    struct FParam // LinearParameter, float kind (0xf0874 / 0xf0ffc)
    {
        float last = 0.5f, cur = 0.5f;
        bool changed() noexcept;
    };
    struct CParam // LinearParameter, choice kind
    {
        int last = 0, cur = 0;
        bool changed() noexcept;
    };
    struct Filter // the container at LoFi+0x4a4 (ctor 0xfc5e4)
    {
        dsp::lofi::Biquad sub[4];
        Gate gate;
        int pending = 3, cur = 3;
        float cutT = 1000.0f, cutC = 1000.0f, cutMult = 0.0f;
        int cutSteps = 0;
        float qT = 0.7f, qC = 0.7f, qMult = 0.0f;
        int qSteps = 0;
        float next = 0.0f;
        int maxSteps = 0;
        bool flag40 = false, dirty41 = false;

        void init(int fs, double fsd) noexcept;
        void setType(int t) noexcept;
        void configure(dsp::lofi::Biquad& s) noexcept;
        void rampSetup() noexcept;
        dsp::lofi::Biquad& swap() noexcept;
        void snapNow() noexcept; // Sixteen start-up (FX-980 fix): pending type and targets apply at once
        void process(float* l, float* r, int n, bool scalar) noexcept;
    };

    void init() noexcept; // the constructor state (prepare, kClearState)
    void applyParams() noexcept;
    void preamp(float* l, float* r, int n) noexcept;
    void crush(float* l, float* r, int n) noexcept;
    void sampleHold(float* l, float* r, int n) noexcept;
    void chain(float* l, float* r, int n) noexcept;
    void applyStartUpProfile() noexcept;

    double fs_ = 44100.0;
    int fsInt_ = 44100;
    FParam p_[6];
    CParam type_, pos_;
    // preamp (+0x498)
    int preN_ = 44, preCnt_ = 0;
    float preT_ = 1.0f, preC_ = 1.0f, preInc_ = 0.0f;
    // crusher (+0x49c)
    int bits_ = 32;
    float scale_ = 256.0f, inv_ = 1.0f / 256.0f;
    // sample-and-hold (+0x4a0)
    float shPhase_ = 1.0f, shStep_ = 1.0f, shHold_[2]{};
    Filter filt_;
    Gate posGate_;
    int posPending_ = 1, posCur_ = 1;
    std::uint32_t quirks_ = 0;
    bool pristine_ = true; // nothing processed since prepare / kClearState
    bool typeSet_ = false, posSet_ = false;
    bool scalar_ = false;
};
} // namespace engine::fx
