// FxProcessor — one FX slot (spec effect-shell.md "Processor requirements" FX-040…051; research
// docs/effects/fx-processors.md, CC BY 4.0). Operation for operation equal to the MIT models tools/fx/fx_processors.py
// (MasterProcessor, SendProcessor, InsertProcessorFull, MuteSmoother) and effectbase.py (InsertProcessor), so the
// processor goldens (tests/golden/fx_shell, subject fx.processor) match them within 1e-6 with the exact swap block.
//
// One instance of every type the slot supports, built and prepared on the message thread (FX-005/040), each wrapped in
// its own FxShell; a type change only switches the index (never allocates):
//
//   requestType(t)   FX-041/042: from Off, t is current at once and activates at the next block; otherwise the current
//                    effect stops (N_shell fade), the processor polls it at each block start and swaps at the first
//                    finished block (3 blocks later at 64 frames), then activates t (reset + fade-in). A later request
//                    re-arms the stop and replaces the pending type. t = −1 is Off.
//   process(L, R)    Insert: dry copy → bypass MuteSmoother → poll/swap → effect → bypass crossfade (FX-044)
//                    Send:   input mute (Off, disabled) → dry copy → poll/swap unless disabled → effect → bypass
//                            crossfade (FX-043/046/047)
//                    Master: dry copy → poll/swap → effect → bypass crossfade (a disabled Master swaps without
//                            activating, FX-046/047)
//                    The tempo is clamped to 1…999 BPM for the effect (FX-049); Send and Master see no triggers
//                    (FX-050: triggerMask 0, offsets −1).
//   setDisabled(b)   FX-047 (FX mode, MIDI tracks): stop fade; re-enable = full re-activation (+ N_mute unmute on
//   Send). idle()           Off (or disabled and settled) with no ramp: process() returns at once — the idle
//   slot.
//
// Plus the Sixteen additions: silenceAndClear() (FX-036 second STOP: output fade over N_mute, kClearState, fade back
// in), retrigger() (FX-089), per-type parameter and option state kept across type changes (FX-006), peak meters.
//
// Threads: build()/prepare() on the message thread (allocate); everything else on the audio thread (noexcept).
#pragma once

#include "engine/fx/EffectBase.h"
#include "engine/fx/FxShell.h"
#include "engine/fx/dsp/Ramps.h"

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace engine::fx
{
class FxProcessor
{
public:
    FxProcessor() = default;
    FxProcessor(const FxProcessor&) = delete;
    FxProcessor& operator=(const FxProcessor&) = delete;

    // Message thread: one instance per type (types in slot order, e.g. listFor(registry, kind)). Re-building drops
    // the previous instances.
    void build(SlotKind kind, std::span<const EffectDescriptor* const> types);
    // Message thread: prepares every instance (allocates their buffers) and every shell. H073/H078: rebuild only on a
    // sample-rate change (stop, prepare, re-activate). Returns false if maxBlock < 64.
    bool prepare(double sampleRate, int maxBlock);
    // After prepare(): a Send slot starts with its input mute open, as the reference processor does, so a slot that
    // starts Off passes one N_mute decaying ramp of the bus (FX-048, reference quirk profile and goldens only).
    void setReferenceStartUp() noexcept;

    void requestType(int type) noexcept; // −1 = Off; out of range = Off
    void setBypass(bool on) noexcept;    // N_mute crossfade to the dry copy (Insert FX-044, Send/Master FX-046)
    void setDisabled(bool on) noexcept;  // FX-047
    void setParam(int type, int k, float v01) noexcept; // per-type state (FX-006), applied now (block start)
    void setOption(int type, int which, int value) noexcept;
    void setQuirkProfile(std::uint32_t mask) noexcept; // every instance, at its next activation (FX-053)
    void retrigger() noexcept;                         // FX-089: current and pending instance
    void silenceAndClear() noexcept;                   // FX-036

    // Exactly 64 frames, in place. Returns false when the output is silence the caller may skip (Send only: Off or
    // disabled and settled; the buffer is then left as it was); true otherwise.
    bool process(float* left, float* right, const ProcessContext& ctx) noexcept;
    bool idle() const noexcept { return idle_; }

    SlotKind kind() const noexcept { return kind_; }
    int typeCount() const noexcept { return static_cast<int>(effects_.size()); }
    const EffectDescriptor* descriptor(int type) const noexcept;
    int current() const noexcept { return cur_; }
    int pending() const noexcept { return pend_; }
    bool disabled() const noexcept { return disabled_; }
    bool bypassed() const noexcept { return bypass_; }
    int latencySamples() const noexcept;
    EffectBase* effect(int type) const noexcept;
    FxShell* shell(int type) noexcept;
    const FxShell* shell(int type) const noexcept;
    float telemetry(int which) const noexcept; // the current effect's (0 when Off)
    double sampleRate() const noexcept { return fs_; }

    // Peak meters of the last block (only computed while enabled: one panel shows them at a time).
    void setMetered(bool on) noexcept { metered_ = on; }
    float inPeak() const noexcept { return inPeak_; }
    float outPeak() const noexcept { return outPeak_; }

    static double clampTempo(double bpm) noexcept; // f32(bpm) clamped to [1, 999] (FX-049; NaN passes)

private:
    bool valid(int t) const noexcept { return t >= 0 && t < static_cast<int>(effects_.size()); }
    void swap() noexcept;
    void run(float* l, float* r, const ProcessContext& ctx) noexcept;
    void updateIdle() noexcept;
    void killStep() noexcept;

    SlotKind kind_ = SlotKind::Insert;
    std::vector<const EffectDescriptor*> types_;
    std::vector<std::unique_ptr<EffectBase>> effects_;
    std::vector<FxShell> shells_;
    double fs_ = 44100.0;
    int cur_ = -1, pend_ = -1;
    bool bypass_ = false, disabled_ = false, idle_ = true, metered_ = false;
    dsp::MuteSmoother ms_{};     // Insert: bypass crossfade; Send: input mute
    dsp::MuteSmoother kill_{};   // FX-036 output fade
    dsp::MuteSmoother byp_{};    // Send/Master: bypass crossfade (FX-046; the effect keeps running behind it)
    std::uint8_t killPhase_ = 0; // 0 none, 1 fading out, 2 fading in
    float inPeak_ = 0.0f, outPeak_ = 0.0f;
    alignas(16) float copyL_[kBlockFrames]{};
    alignas(16) float copyR_[kBlockFrames]{};
    alignas(16) float dryL_[kBlockFrames]{};
    alignas(16) float dryR_[kBlockFrames]{};
};
} // namespace engine::fx
