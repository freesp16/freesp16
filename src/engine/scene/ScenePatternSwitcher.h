// Scenes, patterns and queued switching on the audio thread (spec 02-features/scenes-and-patterns.md
// SCN-001…027).
//
// Resident scenes (SCN-001/002/010): every engine stage keeps [16 scenes][16 tracks] parameter blocks addressed by
// ParamChange.scope = (scene << 4) | track, and every scene's samples are resident (LIB-030), so a switch is an index
// change — nothing is copied. Step 5 of the block (after the transport, before the sequencer) runs process():
//
//   request(s, p) / cancel()   ← scene.queue ParamChange (value = s × 16 + p; −1 cancel; see kQueue*)
//   process(ctx, timing, …)    → BlockBuffers::timing {scene, pattern, patternStartPpq, boundary, sceneChanged}
//                              → Mixer::setScene / setPatternMuteSolo at the boundary frame (SCN-012)
//                              → Transport SetTempo with the new scene's BPM (SCN-004, policy APPLY)
//                              → EngineEvent SwitchDone (value = s × 16 + p) for the message thread
//   state()                    → PlayheadFeed {scene, pattern, step, pendingScene, pendingPattern} (SCN-022 blink)
//
// Boundary contract (read by the sequencer for its split):
//   - A pattern "pass" is [patternStartPpq, patternStartPpq + length × 0.25). timing.patternStartPpq is the start of the
//     pass that contains the block start p0 (wraps without a switch are not boundaries: step s of the pass sits at
//     patternStartPpq + s × 0.25, the sequencer wraps at `length`).
//   - Next boundary B ≥ p0 (all relative to the current pass, never absolute PPQ): STEP = next multiple of 0.25,
//     BAR = next multiple of 4.0 but at most the pass end, PATTERN = the pass end (= patternStartPpq when p0 is
//     exactly the start of a pass). A queued switch lands in this block when p0 ≤ B < p1 (half-open, like the
//     sequencer's events); timing.boundary = {true, frameAt(B), B}.
//   - With a boundary, timing.scene / pattern / patternStartPpq already describe the NEW pattern (valid from
//     boundary.frame on); frames [0, boundary.frame) keep the previous block's values (the sequencer's own memory, or
//     previous()). RESTART (default, SCN-025): the new pass starts at B (its step 1 on boundary.frame). CONTINUE: the
//     step position is kept modulo the new length (patternStartPpq = B − (position mod new length)).
//   - Frame of a PPQ t in the block: frameAt(t, p0, ppqPerFrame) = ⌊(t − p0) / ppqPerFrame + 1e-6⌋ (the epsilon only
//     absorbs double rounding of an exact frame; the sequencer should use the same helper).
//   - Stopped or in the pre-count (nothing plays a pattern): a switch is immediate (SCN-024), at the next block start,
//     with boundary.has = false. Play from the top (start edge with p0 ≤ 0, incl. the pre-count's negative PPQ)
//     anchors the pass at PPQ 0; Continue keeps the pass phase; a locate backwards steps back whole passes.
//
// Our assumptions: the scene BPM is sent as SetTempo when the switch happens, so it governs from
// the next block start (≤ 64 frames, 1.45 ms after the boundary); a scene without a BPM keeps the running tempo; with
// setSwitchable(false) new requests are ignored and a queued switch waits until switching is allowed again.
//
// Real-time: everything here is noexcept, allocation-free, O(1) per block (a few compares and one boundary
// computation; budget ≤ 2 µs per block on the A9, tests/bench/bench_scene.cpp).
#pragma once

#include "engine/EngineApi.h"
#include "engine/stages/BlockBuffers.h"

#include <array>
#include <cstdint>

namespace engine::scene
{
enum class SwitchUnit : std::uint8_t { Step, Bar, Pattern };            // patternQuantize, default Pattern (SCN-020)
struct SwitchState { std::uint8_t scene, pattern; std::int8_t pendingScene, pendingPattern; double patternStartPpq; };

constexpr int kScenes = 16, kPatterns = 16, kMaxSteps = 64;
constexpr double kQuartersPerStep = 0.25, kQuartersPerBar = 4.0; // 4/4 only (SEQ-002)

// scene.queue values (internal ParamSpec, never saved; core/params/catalogue/scene.cpp). A request s × 16 + p may add
// kQueueApplyBpm: "apply the new scene's BPM at this switch" whatever the policy (the ASK popup answered yes, SCN-005).
constexpr int kQueueCancel = -1, kQueueBlock = -2, kQueueUnblock = -3, kQueueApplyBpm = 256;

// Frame of PPQ `t` inside a block that starts at `p0` (see the boundary contract above), clamped to 0…63.
constexpr int frameAt(double t, double p0, double ppqPerFrame) noexcept
{
    if (! (ppqPerFrame > 0.0))
        return 0;
    const double f = (t - p0) / ppqPerFrame + 1e-6;
    const int i = f < 0.0 ? 0 : static_cast<int>(f);
    return i > kBlockFrames - 1 ? kBlockFrames - 1 : i;
}

// What one block's process() changed (for the side effects and for tests).
struct SwitchResult
{
    bool switched = false;      // a switch happened this block (at `frame`)
    bool sceneChanged = false;  // … and it changed the scene
    bool masksChanged = false;  // the pattern mute/solo masks to send to the mixer (at `frame`)
    int frame = 0;
    float tempo = 0.0f;         // > 0: SetTempo to send (scene BPM, SCN-004)
};

class ScenePatternSwitcher
{
public:
    ScenePatternSwitcher() noexcept { reset(); }
    void reset() noexcept; // scene 1 pattern 1, nothing pending, defaults (16 steps, no masks, PATTERN, RESTART, APPLY)

    // ---- from the FIFO drain (audio thread, step 1) ----
    void request(int scene, int pattern) noexcept { request(scene, pattern, false); }
    void request(int scene, int pattern, bool applyBpm) noexcept; // the current pattern again = cancel (SCN-021)
    void cancel() noexcept;                                       // SCN-021
    void applyQueue(float value) noexcept;                        // a scene.queue value (kQueue*)
    void setSwitchable(bool) noexcept;                            // SCN-023: false while SAMPLING/EDIT is open
    void setUnit(SwitchUnit) noexcept;
    void setContinue(bool) noexcept;                              // SCN-025: false = RESTART (default)
    void setSceneBpm(int scene, float bpmOrZero) noexcept;        // SCN-004: 0 = the scene has no BPM
    void setFollowSceneBpm(bool) noexcept;                        // policy APPLY (ASK / IGNORE: false)
    void setPatternLength(int scene, int pattern, int steps) noexcept; // 1–64 (the UI enforces 8–64)
    void setPatternMuteSolo(int scene, int pattern, std::uint16_t mute, std::uint16_t solo) noexcept; // SCN-011/012

    // ---- step 5 ----
    // Advances the switch state over the block [ctx.ppqStart, ctx.ppqEnd) and fills `timing` (scene, pattern,
    // patternStartPpq, boundary, sceneChanged; preCount is read — the transport fills it in step 3).
    SwitchResult advance(const BlockContext& ctx, stages::BlockBuffers::Timing& timing) noexcept;

    // advance() plus its side effects: the mixer's scene and pattern masks at the switch frame, SetTempo, SwitchDone.
    // Transport: command(const TransportCmd&); Mixer: setScene(int, int), setPatternMuteSolo(u16, u16, int);
    // Events: pushEvent(const EngineEvent&) (engine::feeds::EngineFeeds). Templates so tests can pass fakes.
    template <class Transport, class Mixer, class Events>
    SwitchResult process(const BlockContext& ctx, stages::BlockBuffers::Timing& timing, Transport& transport,
                         Mixer& mixer, Events& events) noexcept
    {
        const SwitchResult r = advance(ctx, timing);
        if (r.sceneChanged)
            mixer.setScene(state_.scene, r.frame);
        if (r.masksChanged)
            mixer.setPatternMuteSolo(mute_[state_.scene][state_.pattern], solo_[state_.scene][state_.pattern], r.frame);
        if (r.tempo > 0.0f)
            transport.command(TransportCmd {TransportCmd::SetTempo, static_cast<double>(r.tempo)});
        if (r.switched)
        {
            EngineEvent e {};
            e.kind = EngineEvent::Kind::SwitchDone;
            e.step = static_cast<std::uint8_t>(r.frame);
            e.value = static_cast<std::int16_t>(state_.scene * kPatterns + state_.pattern);
            events.pushEvent(e);
        }
        return r;
    }

    // ---- reads (audio thread; the PlayheadFeed is published by step 14) ----
    const SwitchState& state() const noexcept { return state_; }
    const SwitchState& previous() const noexcept { return previous_; } // before this block's switch (the split)
    int patternLength(int scene, int pattern) const noexcept { return length_[scene & 15][pattern & 15]; }
    int currentLength() const noexcept { return length_[state_.scene][state_.pattern]; }
    std::uint16_t patternMute(int scene, int pattern) const noexcept { return mute_[scene & 15][pattern & 15]; }
    std::uint16_t patternSolo(int scene, int pattern) const noexcept { return solo_[scene & 15][pattern & 15]; }
    float sceneBpm(int scene) const noexcept { return sceneBpm_[static_cast<std::size_t>(scene & 15)]; }
    bool continues() const noexcept { return continue_; }
    bool followsSceneBpm() const noexcept { return followBpm_; }
    SwitchUnit unit() const noexcept { return unit_; }
    bool switchable() const noexcept { return switchable_; }
    // The step (0-based, within the current pattern) at transport position `ppq`; 0 before the pass starts.
    int stepAt(double ppq) const noexcept;
    // Fills the switcher's PlayheadFeed fields: scene, pattern, step (at `ppq`), pendingScene, pendingPattern.
    void publish(PlayheadFeed& feed, double ppq) const noexcept;

private:
    double nextBoundary(double p0) const noexcept;
    void switchTo(double at, int frame, SwitchResult& r) noexcept;
    double lengthQuarters(int scene, int pattern) const noexcept
    {
        return static_cast<double>(length_[scene][pattern]) * kQuartersPerStep;
    }

    SwitchState state_ {}, previous_ {};
    bool applyBpm_ = false;          // the pending request carries kQueueApplyBpm
    bool switchable_ = true, continue_ = false, followBpm_ = true;
    bool masksDirty_ = true;         // the current pattern's masks changed (or never sent): send at the next block
    SwitchUnit unit_ = SwitchUnit::Pattern;
    std::array<float, kScenes> sceneBpm_ {};
    std::uint8_t length_[kScenes][kPatterns] {};
    std::uint16_t mute_[kScenes][kPatterns] {}, solo_[kScenes][kPatterns] {};
};
} // namespace engine::scene
