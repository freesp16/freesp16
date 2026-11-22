// engine::mixer::HeadphoneRouter — the headphone cue (spec 02-features/mixer-and-routing.md MIX-040…
// MIX-042, OQ-17; research docs/effects/signal-flow.md "Main output and headphones": HeadphoneUnit / HeadphoneRouter).
//
//   phones = selected cue × selector fade  +  monitor buses × gate  →  × HEADPHONES level  (→ trim and mute-all in the
//            output stage, MIX-023)
//   cue        MASTER (the master, post master fader), OUT 1-2…7-8 (the router's pairs) or a mono channel 1…8 of the
//              router duplicated to both ears (MIX-040). Per scene (the stage applies the current scene's). MASTER and
//              OUT 1-2 are one selector input, the router's OUT 1-2 (the master in M + 6CH), as in the reference (F209:
//              CueMaster and Cue12 both select in2/3): a mode change that turns one into the other does not fade the
//              headphones, the router's own dip does.
//   valid cues M + 6CH: MASTER, OUT3-4, OUT5-6, OUT7-8, 3…8; 8CH: OUT1-2…7-8, 1…8 (the 1.60 lists, F211). An invalid
//              cue is corrected while the mode makes it invalid (our choice: M + 6CH OUT1-2 / 1 / 2 → MASTER, 8CH
//              MASTER → OUT1-2); the stored value is kept, so it comes back with the mode.
//   fades      a cue change is a 128-sample fade-out of the old source and a 128-sample fade-in of the new one
//              (MIX-042): the research Selector (routing::SelectorFade) operation for operation, so a change during a
//              fade-in fades out over 128 samples from the gain reached. Reference quirk (F209): re-selecting the
//              source that is fading out lets the fade-out finish and leaves the selector at gain 0 until the next
//              change — reproduced only with setReferenceQuirk(true) (FX-052 reference profile); the app fades back in
//              from the gain reached instead.
//   monitors   preview + metronome (the aux bus) and the input monitor are added at unity (MIX-041), each behind a
//              44-sample gate (the research MuteSmoother(44), F210) that is closed while the cue is MASTER (the master
//              already carries them in the app). App: the audible cue decides; reference profile: the selected
//              one, from the change on (the reference's CueMaster flag is set with the selection).
//   level      headphoneVolume 0–127, the fader law × 0.5 (127 = unity, 100 = −6 dB), 100-sample linear ramp.
//
// Threads: prepare() on the message thread; the rest on the audio thread. No allocation, no virtual call; per-sample
// work in OutputRouterKernels.h. Budget ≤ 400 A9 cycles per block, nothing while the Phones port is absent.
#pragma once

#include "engine/EngineApi.h"
#include "engine/mixer/OutputRouter.h"
#include "engine/mixer/OutputRouterKernels.h"
#include "engine/mixer/Ramps.h"

#include <cstdint>

namespace engine::mixer
{
// core::model::HeadphoneCue order (stored CueMaster, Cue12 … Cue78, Cue1 … Cue8).
enum class Cue : std::uint8_t { Master, Out12, Out34, Out56, Out78, Mono1, Mono2, Mono3, Mono4, Mono5, Mono6, Mono7, Mono8 };
constexpr int kCues = 13;

// What the headphones add on top of the cue (null = silent this block).
struct MonitorBuses
{
    const float* auxL = nullptr; // preview + metronome (BlockBuffers::aux, MIX-041)
    const float* auxR = nullptr;
    const float* inputL = nullptr; // the input monitor (InputMonitor)
    const float* inputR = nullptr;
};

// The cue the headphones play for `requested` in `mode` (MIX-040 lists; our correction of an invalid cue).
constexpr Cue effectiveCue(Cue requested, AssignMode mode) noexcept
{
    if (mode == AssignMode::MasterPlus6)
        return requested == Cue::Out12 || requested == Cue::Mono1 || requested == Cue::Mono2 ? Cue::Master : requested;
    return requested == Cue::Master ? Cue::Out12 : requested;
}

class HeadphoneRouter
{
public:
    static constexpr int kFadeFrames = 128; // MIX-042
    static constexpr int kGateFrames = 44;  // F210

    HeadphoneRouter() noexcept;
    void prepare(double sampleRate) noexcept; // message thread: every fade and ramp settles on its target

    // ---- audio thread (block start) ----
    void setCue(int cueIndex) noexcept;     // core::model::HeadphoneCue index 0–12
    void setMode(AssignMode) noexcept;      // corrects an invalid cue (and restores it)
    void setLevel(float index0to127) noexcept;
    void setReferenceQuirk(bool on) noexcept { quirk_ = on; }

    // One block into l / r (64 frames, overwritten). out = the router's pairs (pre-trim; MASTER plays its OUT 1-2, the
    // master in M + 6CH); `master` is not read (kept for the stage's call).
    void process(const OutputBuffers& out, const float* const* master, const MonitorBuses& mon, float* l,
                 float* r) noexcept;

    // ---- inspection ----
    Cue requested() const noexcept { return requested_; }
    Cue audible() const noexcept { return active_; } // the cue whose source plays (fading in or out)
    bool fading() const noexcept { return curSrc_ != tgtSrc_ || ! sel_.idle(); }
    bool stuck() const noexcept // the reference quirk's gain-0 state
    {
        return curSrc_ == tgtSrc_ && sel_.idle() && sel_.current() == 0.0f;
    }
    float level() const noexcept { return level_.current(); }

    // The research MuteSmoother (fx_processors.py, 0xe7fd8 / 0xe808c) as a gain source: N-sample linear fade, step
    // ±F(1/N) accumulated in float; a change mid-fade turns round from the gain reached (remaining = N − remaining).
    class Gate
    {
    public:
        enum class Kind : std::uint8_t { Open, Closed, Buffer };
        void reset(bool muted) noexcept;
        void set(bool muted) noexcept;
        Kind fill(float* out, int n) noexcept; // Buffer: out[0, n) written

    private:
        static inline const float kInv = static_cast<float>(1.0 / static_cast<double>(kGateFrames));
        float g_ = 1.0f, step_ = -kInv;
        int rem_ = 0;
        std::uint8_t state_ = 0; // 0 open, 1 ramp, 2 closed
        bool muted_ = false;
    };

private:
    void retarget(Cue c) noexcept;

    Cue requested_ = Cue::Master, active_ = Cue::Master, pending_ = Cue::Master;
    AssignMode mode_ = AssignMode::MasterPlus6;
    int curSrc_ = 0, tgtSrc_ = 0; // selector inputs: 0…3 = OUT 1-2 (= MASTER) … 7-8, 4…11 = mono channel 1…8
    bool quirk_ = false;
    routing::SelectorFade sel_;
    Gate gate_;
    GainRamp level_;
    alignas(16) float selGain_[kBlockFrames] {};
    alignas(16) float gateGain_[kBlockFrames] {};
    alignas(16) float levelGain_[kBlockFrames] {};
};
} // namespace engine::mixer
