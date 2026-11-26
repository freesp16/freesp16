// engine::sampling::LiveSampler — live sampling while the sequencer plays (spec 02-features/sampling.md
// SMP-020…027). Audio thread only, O(1) per block, no allocation.
//
//   arm(track, length, loop)   live.rec = 1 (SMP-022): Armed; the take starts at the next pattern-quantize boundary
//                              (project.patternQuantize: STEP / BAR / PATTERN, the switcher's rule — boundaryFrame())
//   schedule(…)                step 6, before the trigger router: Armed + a boundary in this block → Recording with the
//                              ring window start = head + frame + latency (SMP-027: LINE IN arrives `latency` frames
//                              after the transport frame it belongs to; 0 for RESAMPLE), end = start + length exactly
//                              (length = bars × 4 × 60 / BPM × fs, rounded once). LoopPending + a boundary → one Full
//                              trigger of the track at that frame with locks START = LOOP START = margin + nudge,
//                              LENGTH = the take, LOOP on, pitch 0, stretch OFF, reverse off (the take buffer starts one
//                              beat — `margin` — before the window, services::sampling::TakeBuilder) → Looping.
//   afterWrite(head)           step 13, after the ring write: Recording and head ≥ end → Done (the window is complete;
//                              the session builds the take buffer on a worker).
//   takeReady(track, seq)      live.takeReady: the take buffer of take `seq` is installed in the track's TrackSample
//                              slot (posted before this ParamChange, FIFO order) → LoopPending when loop-after-rec is
//                              on (SMP-023). A buffer that is late simply waits for the next boundary — never mid-step.
//   setNudge(frames)           SMP-024: ±1 frame … ±1 beat (clamped to the margin), moves the window only (no copy);
//                              while Looping the loop restarts with the new window at the next boundary.
//
// The loop starts at the first boundary after the buffer is ready (ready within one 1/16 at 300 BPM);
// the loop note plays until the track is triggered again (sequencer, pad) or the take is unloaded.
#pragma once

#include "engine/EngineApi.h"
#include "engine/sampling/CaptureRing.h"
#include "engine/scene/ScenePatternSwitcher.h"
#include "engine/stages/BlockBuffers.h"
#include "engine/trigger/TriggerEvent.h"

#include <array>
#include <cstdint>

namespace engine::trigger
{
class TriggerRouter;
}

namespace engine::sampling
{
// The frame (0–63) of the first pattern-quantize boundary in this block, −1 when none (stopped, pre-count, or no
// boundary). A queued pattern switch landing here is a boundary too. Same rule as ScenePatternSwitcher: STEP = next
// multiple of 0.25 PPQ of the pass, BAR = next multiple of 4 (at most the pass end), PATTERN = the pass end (or its
// start when p0 is exactly there).
int boundaryFrame(const BlockContext& ctx, const stages::BlockBuffers::Timing& timing, scene::SwitchUnit unit,
                  int patternSteps) noexcept;
// REC QUANTIZE (SAMPLING/EDIT): the frame (0–63) of the first multiple of `beats` quarter notes from the pass start
// in this block, capped at the pass end (a 16-beat wait on a 2-bar pattern is the next pattern start), −1 when none.
// A queued pattern switch landing here is one too. beatsLeft = quarter notes from this block's start to that boundary
// (0 … beats), −1 when stopped or in the pre-count (the take then waits for PLAY).
int gridFrame(const BlockContext& ctx, const stages::BlockBuffers::Timing& timing, double beats, int patternSteps,
              double& beatsLeft) noexcept;

// Record lengths of live.length (SMP-021): 1/4, 1/2, 1, 2, 4, 8 bars.
constexpr float kLiveBars[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f};
// Frames of `bars` 4/4 bars at `bpm` (rounded to the nearest frame).
std::int64_t barsToFrames(double bars, double bpm, double sampleRate) noexcept;

class LiveSampler
{
public:
    void prepare(double sampleRate) noexcept;
    void reset() noexcept; // Idle (a new hub)

    // ---- audio thread, step 1 ----
    void setMode(bool on) noexcept;            // SMP-020; off disarms an armed track (a running take completes)
    bool mode() const noexcept { return mode_; }
    void setLengthIndex(int index) noexcept;   // live.length 0–5
    void setLoopAfterRec(bool on) noexcept { loopAfterRec_ = on; }
    // Arm `track` (scene for the take buffer slot); lengthFrames ≤ 0 = from live.length at the start tempo.
    void arm(int scene, int track, std::int64_t lengthFrames, bool loopAfterRec) noexcept;
    void arm(int track, std::int64_t lengthFrames, bool loopAfterRec) noexcept { arm(scene_, track, lengthFrames, loopAfterRec); }
    void disarm() noexcept;                    // live.rec = 0: Armed → Idle; Recording → stops at once (shorter take)
    void stopPlayback() noexcept;              // restart a playing take on the next transport boundary
    void setNudge(std::int32_t frames) noexcept;
    void takeReady(int track, std::uint32_t seq) noexcept;
    void resolve(int token) noexcept; // saved take sequence (low 24 bits), or a negative reset token

    // ---- step 6 (before TriggerRouter::process) ----
    // frame = boundaryFrame() of this block (−1 none); head = ring head at the block start; latency = frames the
    // source lags the transport (SMP-027). router null = no loop trigger (tests).
    void schedule(const BlockContext& ctx, int frame, std::int64_t head, int latency, trigger::TriggerRouter* router,
                  CaptureStatus* status) noexcept;
    // ---- step 13 ----
    void afterWrite(std::int64_t head, CaptureStatus* status) noexcept;

    LiveState state() const noexcept { return state_; }
    const LiveTake& take() const noexcept { return take_; }
    std::uint32_t seq() const noexcept { return seq_; }
    std::int32_t margin() const noexcept { return margin_; }
    std::int64_t lastLoopFrame() const noexcept { return loopFrame_; }
    int scene() const noexcept { return scene_; }

private:
    void publish(CaptureStatus* s) const noexcept;

    double sampleRate_ = 44100.0;
    bool mode_ = false, loopAfterRec_ = false;
    bool unsaved_ = false;
    std::uint32_t resetToken_ = 0;
    int lengthIndex_ = 2;
    LiveState state_ = LiveState::Idle;
    LiveTake take_ {};
    int scene_ = 0;
    std::int64_t armedLength_ = 0;   // frames requested by arm() (0 = from the tempo)
    bool armedLoop_ = false;
    std::int32_t margin_ = 0;        // one beat at the take's tempo
    std::int32_t pendingNudge_ = 0;  // the last setNudge (applies to the next or running take)
    double bpm_ = 120.0;
    float bars_ = 1.0f;
    std::uint32_t seq_ = 0;
    int latency_ = 0;
    std::int64_t loopFrame_ = -1;
    bool statusDirty_ = true;
    // The loop note's locks (TriggerEvent::locks; the LockSet stays valid while the router uses it: this block).
    trigger::LockSet locks_ {};
    std::array<float, 35> lockValues_ {};
    std::array<std::uint8_t, 35> lockChange_ {};
};
} // namespace engine::sampling
