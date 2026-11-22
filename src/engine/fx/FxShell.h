// FxShell — the wrapper around every effect instance (spec 02-features/effects/effect-shell.md FX-020…057; research
// docs/effects/effectbase.md "The slot-18 wrapper", CC BY 4.0). Operation for operation equal to the MIT model
// tools/fx/effectbase.py (EffectBase.slot5/6/7/8/9/18/31, InOutGain, IirHpf1stOrder, the SignalGate pair), so the
// shell goldens (tests/golden/fx_shell) match it within 1e-6.
//
//   process(L, R), in place, exactly 64 frames:
//     pending gate target → if not running: return (buffers untouched; FX-023 before the first activation)
//     [reference profile, Insert only: skip N_shell samples after an activation / clamp to N_shell after a stop]
//     copy = input · inputGain(t)                 gate mode 1 only (FX-027)
//     effect.process(copy)                        the effect writes its WET signal into the copy (FX-020)
//     copy = Gate → HPF 10 Hz → Gate              ShellPolicy::wetHighPass (FX-028/029)
//     out = f32(f32(in·dry(t)) + f32(copy·wet(t)))  non-fused (FX-030), glides over N_shell (FX-031)
//
// The behaviour is data: the descriptor's ShellPolicy (engine/fx/EffectDescriptor.h, frozen) carries
//   gateMode      0 = Open (input never faded), 1 = InputFade (Insert Delay, Send/Master Delay, Reverb)
//   mixLaw        a MixLaw value below (FX-030 table); the shell recomputes dry/wet on every setParam (FX-031)
//   wetHighPass   the 10 Hz post-stage (Chorus, Flanger, Phaser, Filter, LoFi, Distortion)
//   skipWhenSilent / clampOutput  the reference skip (after activation) and clamp (after stop) of FX-033; honoured
//                 only in an Insert slot with the kQuirkShellSkipClamp bit of the quirk profile set (our choice: the
//                 effect still processes all 64 frames — the frozen EffectBase has no sub-block length — and the shell
//                 mixes only the unskipped part, so a stateful effect sees the skipped samples; FX-033 is P2)
//
// Threads: prepare() on the message thread (no allocation here; the effect allocates in its own prepare); everything
// else on the audio thread, noexcept, allocation- and lock-free. One FxShell per effect instance (one per type per
// slot, FX-005); the copy buffers are the owning FxProcessor's scratch (only one shell of a slot runs per block).
#pragma once

#include "engine/EngineApi.h"
#include "engine/fx/EffectBase.h"
#include "engine/fx/dsp/Filters.h"
#include "engine/fx/dsp/Ramps.h"

#include <cstdint>

namespace engine::fx
{
enum class GateMode : std::uint8_t
{
    Open = 0,
    InputFade = 1
}; // ShellPolicy::gateMode (FX-027)

// ShellPolicy::mixLaw (FX-030): how the shell derives dry/wet from the six stored parameters (k5 = Mix).
enum class MixLaw : std::uint8_t
{
    WetOnly = 0,         // dry 0, wet 1 (2-band EQ, Ducker; also the ShellPolicy{} default, GainTest)
    UnityDryK5 = 1,      // dry 1, wet = v (Chorus, Flanger)
    CrossfadeK5 = 2,     // dry 1 − v, wet v (Phaser, Filter, LoFi, Distortion; Compressor with make-up in its wet)
    DelayPiecewise = 3,  // v < 0.75: dry 1, wet 4v/3; else dry 1 − 4(v − 0.75), wet 1 (Insert and Send/Master Delay)
    ReverbPiecewise = 4, // the delays' law, wet × 0.70794 (Reverb)
    EffectDefined = 5,   // the effect reports dry/wet: telemetry(kTelemetryDry/kTelemetryWet), read every block
    Count
};
using MixLawFn = void (*)(const float* params6, float& dry, float& wet) noexcept;
namespace mix
{
void wetOnly(const float*, float& dry, float& wet) noexcept;
void unityDryWetK5(const float*, float& dry, float& wet) noexcept;
void crossfadeK5(const float*, float& dry, float& wet) noexcept;
void delayPiecewise(const float*, float& dry, float& wet) noexcept;
void reverbPiecewise(const float*, float& dry, float& wet) noexcept;
} // namespace mix
MixLawFn mixLawFor(std::uint8_t mixLaw) noexcept; // null for EffectDefined or an unknown value (→ dry 1, wet 0)

// Conventional effect option codes (EffectBase::setOption 0…99 are the effect's own; these four are the slot fields
// of model::EffectSlot every effect that has them uses). Codes >= 100 are EffectBase.h's ReservedOption.
enum FxOption : int
{
    kSync = 0,
    kType1 = 1,
    kType2 = 2,
    kLookAhead = 3
};
// telemetry() codes the framework reads (0…99 are the effect's own visuals, UID-834).
enum FxTelemetry : int
{
    kTelemetryDry = 100,
    kTelemetryWet = 101
};

// Quirk profile (FX-052/053): one mask per project, honoured at each effect's next activation. Every documented
// reference quirk is a bit; the Reference profile sets all of them (goldens), the Sixteen profile none.
constexpr std::uint32_t kQuirkShellSkipClamp = 1u << 31; // FX-033
constexpr std::uint32_t kQuirkProfileReference = 0xffffffffu;
constexpr std::uint32_t kQuirkProfileSixteen = 0u;

class FxShell
{
public:
    static constexpr int kMaxOptions = 8;

    // Message thread. `scratch` = two 64-float copy buffers owned by the processor (16-byte aligned).
    void prepare(EffectBase& effect, const ShellPolicy& policy, double sampleRate, SlotKind kind, float* scratchL,
                 float* scratchR) noexcept;

    // Reference slot 5 + slot 7 (FX-024/032/056): send the full parameter set, reset() the effect, read its dry/wet,
    // zero the high-pass, then fade in over N_shell. honourSkip = false for the Send processor (reference).
    void activate(bool honourSkip = true) noexcept;
    void stop() noexcept;          // slot 6 (FX-025): dry → 1, wet → 0 over N_shell; the effect keeps running
    void stopWet() noexcept;       // slot 8 (FX-034 "Off, let tails ring": mode 1 closes the input, keeps the wet)
    bool running() const noexcept; // slot 9: false once a stop fade has completed (and before the first activation)
    bool everActivated() const noexcept { return everActivated_; }

    void setParam(int k, float v01) noexcept;      // clamps to [0, 1] (FX-057), forwards, retargets dry/wet
    void setOption(int which, int value) noexcept; // stored (0…7) and forwarded; >= 100 forwarded only
    void setQuirkProfile(std::uint32_t mask) noexcept { pendingQuirks_ = mask; } // at the next activation (FX-053)
    void clearOnNextActivation() noexcept { clearPending_ = true; }              // FX-036 for inactive types
    void clearNow() noexcept; // kClearState + full parameter set + reset (FX-036)

    // Exactly 64 frames, in place. Returns false when it returned at once (not running, or a whole-block skip).
    bool process(float* left, float* right, const ProcessContext& ctx) noexcept;

    float param(int k) const noexcept { return params_[k]; }
    float dry() const noexcept { return dry_; }
    float wet() const noexcept { return wet_; }
    EffectBase* effect() const noexcept { return effect_; }

    static int shellFadeSamples(double fs) noexcept { return dsp::shellFadeSamples(fs); } // 147 / 160 / 320
    static int muteFadeSamples(double fs) noexcept { return dsp::muteFadeSamples(fs); }   // 132 / 144 / 288

private:
    struct Record // a gate target: per gain (input, dry, wet) a mode (-1 unused, 0 snap, 1 ramp) and a value
    {
        std::int8_t inMode = 0;
        float in = 0.0f;
        std::int8_t dryMode = 0;
        float dry = 0.0f;
        std::int8_t wetMode = 0;
        float wet = 0.0f;
    };
    struct Gate // EffectInOutGain: three SmoothFloats and the 3-state machine (effectbase.py InOutGain)
    {
        dsp::SmoothFloat in, dry, wet;
        int cur = 0, trans = 0, tgt = 0, pend = 0, pstate = 0;
        Record prec{};
        float c0 = 1.0f, c4 = 0.0f;
        bool mode1 = false;
        void prepare(double fs, bool inputFade) noexcept;
        void start(int state, const Record& r) noexcept;
        void reset(const Record& r) noexcept;
        void applyPending() noexcept;
        int running() const noexcept;
        void mix(float* l, float* r, float* cl, float* cr, int n) noexcept;
    };

    void updateMix() noexcept;
    void buildRecords() noexcept; // slot 31
    void post(float* cl, float* cr, int n) noexcept;
    void sendState() noexcept;

    EffectBase* effect_ = nullptr;
    ShellPolicy policy_{};
    MixLawFn law_ = nullptr;
    SlotKind kind_ = SlotKind::Insert;
    double fs_ = 44100.0;
    int fade_ = 147;
    float* cl_ = nullptr;
    float* cr_ = nullptr;

    float params_[6]{};
    int options_[kMaxOptions]{};
    std::uint32_t optionSet_ = 0;
    std::uint32_t pendingQuirks_ = kQuirkProfileSixteen;
    bool clearPending_ = false, everActivated_ = false;

    float dry_ = 1.0f, wet_ = 0.0f;
    bool skipEn_ = false, clampEn_ = false;
    bool skipArmed_ = false, clampArmed_ = false;
    int skip_ = 0, clamp_ = 0;
    Gate gate_{};
    bool hpfReq_ = false, hpfCur_ = false;
    float hpfFc_ = 10.0f;
    dsp::Hpf1 hpf_{};
    dsp::SignalGate gateA_{}, gateB_{};
    Record recs_[5]{};
};
} // namespace engine::fx
