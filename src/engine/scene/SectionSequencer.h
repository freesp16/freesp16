// Song mode on the audio thread (spec 02-features/arrangement.md ARR-001…008, 03-ui/screens.md ARRANGER
// UI-300…306). Runs in step 5 between the pattern chain and the scene switcher
// (engine/scene/ScenePatternSwitcher.h), and drives the switcher through its boundary contract — it never computes a
// boundary of its own: the pass end comes from PatternChainPlayer::patternEndInBlock (the same arithmetic as the
// switcher's advance), and a request made in that block lands exactly on it for every PATTERN QUANTIZE unit.
//
//   process(ctx, timing, switcher, transport, plan)   before ScenePatternSwitcher::process
//     stopped at the top   cue the start row: its scene/pattern now (a stopped switch is immediate, SCN-024) and its
//                          fixed BPM, so PLAY starts the song on its first sample at its tempo
//     start edge           the cued row plays (pass start = the switcher's, read in afterSwitch)
//     pattern end (B)      onPatternEnd: count the pass; when the row's repeats are done, queue the next row's
//                          scene/pattern for B (same scene/pattern: nothing to queue, the pattern wraps on B); after
//                          the last row stop (Stop to 0, default) or loop to row 1 (ARR-005)
//   afterSwitch(ctx, timing, transport, tempoBefore)  after ScenePatternSwitcher::process
//     a row began this block → SetTempo(the row's fixed BPM, or the tempo in force before it for "--": the scene BPM
//     the switcher may have applied does not govern in song mode, ARR-002, our choice); publishes the SongStatus
//
// Rules (our choices):
//   - The BPM step lands at the next block start after the row's first sample (≤ 63 frames, 1.4 ms: the transport has
//     one tempo per block, as for the scene BPM, SCN-004) — except the first row, whose BPM is set while stopped.
//   - Song mode is "on" when playArrangement is set and the installed plan has rows. Then manual scene/pattern requests
//     are refused (refusesSwitch, ARR-007), the pattern chain is suspended, and the stopped switcher shows the cued
//     row. Turning it on while playing enters the cued row at the next pattern end; turning it off leaves the current
//     pattern playing.
//   - startFrom(row) (SHIFT + push, ARR-004): stopped / paused → cue that row (PLAY / CONTINUE plays it); playing →
//     jump to it at the next pattern end. The cue goes back to row 1 whenever the transport stops at the top.
//   - locate16th(pos) (incoming SPP in song mode, ARR-008): the row and pass that contain pos are cued (stopped) or
//     jumped to at the next pattern end (playing); the position snaps to that pass's start.
//   - A new plan keeps the playing row index and its passes left (clamped to the new repeats); rows past the new end
//     end the song at the next pattern end. While switching is blocked (SCN-023) a row that needs a switch holds.
//   - Song end with stop: the transport stops at the block after the end boundary (≤ 63 frames of the wrapped pass).
//
// Song position (SPP out, ARR-008): startPpq16[row] + passes done × pass length + sixteenths into the pass, so at every
// row start it equals the arrangement plan's start table entry.
//
// Real-time: noexcept, no allocation; per block O(1) (a few compares, one boundary computation while the song plays,
// one request per row change). The row lookup of locate16th is a binary search (≤ 10 steps, only on a locate).
#pragma once

#include "core/params/ParamId.h"
#include "core/util/arrangement/ArrangementPlan.h"
#include "engine/EngineApi.h"
#include "engine/bridge/ParamBindings.h"
#include "engine/scene/PatternChain.h"
#include "engine/scene/ScenePatternSwitcher.h"
#include "engine/stages/BlockBuffers.h"

#include <cstdint>
#include <functional>
#include <string_view>

namespace engine::scene
{
using Section = core::arrangement::Section;
using ArrangementPlan = core::arrangement::ArrangementPlan;
inline constexpr SwapSlot kArrangementSlot {SwapSlot::SceneConfig, core::arrangement::kSwapScene,
                                            core::arrangement::kSwapIndex};
inline void destroyArrangementPlan(const void* plan) noexcept { core::arrangement::destroyArrangementPlan(plan); }

// ParamSpec ids the sequencer takes (core/params/catalogue/arrangement.cpp; bound to ParamTarget::Sequencer like the
// perf stage's, Engine::apply offers them to onParam first).
inline constexpr std::string_view kPlayId = "arrangement.play";   // 0/1, ProjectSettings::playArrangement (ARR-004)
inline constexpr std::string_view kLoopId = "arrangement.loop";   // 0 stop / 1 loop at the end (preference, ARR-005)
inline constexpr std::string_view kStartId = "arrangement.start"; // internal: row ≥ 0 = startFrom(row)

struct RowState
{
    std::int16_t row = -1;        // the playing row, −1 = the song does not play
    std::uint8_t remaining = 0;   // passes left in it, the current one included (ARR-007: "1 / 2" = remaining / repeats)
    std::uint8_t repeats = 0;
    std::int16_t cued = 0;        // the row PLAY starts from while the song does not play
};

class SectionSequencer
{
public:
    SectionSequencer() noexcept { reset(); }
    void reset() noexcept; // no plan, song mode off, stop at the end, cue row 1

    // ---- set-up (message thread) ----
    void bindParams(const std::function<core::ParamId(std::string_view)>& resolve, // rt-ok: set-up only
                    bridge::ParamBindings& bindings) noexcept;

    // ---- step 1 (FIFO drain) ----
    bool onParam(const ParamChange&) noexcept;                // arrangement.*: applied, true
    bool refusesSwitch(float queueValue) const noexcept;      // a manual scene.queue request while song mode is on
    void install(const ArrangementPlan*) noexcept;            // installed in kArrangementSlot (process() calls it)
    void setSongMode(bool) noexcept;                          // ARR-004
    void setLoop(bool) noexcept;                              // ARR-005
    void startFrom(int row) noexcept;                         // SHIFT + push (ARR-004)
    void locate16th(std::int64_t pos) noexcept;               // incoming SPP in song mode (ARR-008)

    // ---- step 5 ----
    // Transport: command(const TransportCmd&), bpm() (the stored tempo). Templates so tests can pass fakes.
    template <class Transport>
    void process(const BlockContext& ctx, const stages::BlockBuffers::Timing& timing, ScenePatternSwitcher& sw,
                 Transport& transport, const ArrangementPlan* plan) noexcept
    {
        if (plan != plan_)
            install(plan);
        if (advance(ctx, timing, sw))
            transport.command(TransportCmd {TransportCmd::Stop, 1.0}); // the end of the song (ARR-005 stop)
    }
    // ARR-006: the switcher's pass ended at `at` (this block): count it, queue the next row for that boundary.
    // Returns true when the song ended (stop). Called by process(); public for tests.
    bool onPatternEnd(ScenePatternSwitcher& sw, double at) noexcept;
    template <class Transport>
    void afterSwitch(const BlockContext& ctx, const stages::BlockBuffers::Timing& timing, Transport& transport,
                     double tempoBefore) noexcept
    {
        double tempo = 0.0;
        if (settle(ctx, timing, tempoBefore, transport.bpm(), tempo))
            transport.command(TransportCmd {TransportCmd::SetTempo, tempo});
    }

    // ---- reads (audio thread; tests after the device stopped) ----
    bool songMode() const noexcept { return songMode_; }
    bool engaged() const noexcept { return songMode_ && plan_ != nullptr && plan_->count > 0; } // song mode is on
    bool suspendsChain() const noexcept { return engaged(); } // the chain waits while song mode is on
    bool active() const noexcept { return active_; }          // the song plays (a row is entered)
    bool loops() const noexcept { return loop_; }
    const ArrangementPlan* plan() const noexcept { return plan_; }
    RowState playing() const noexcept;                         // ARR-007
    std::int64_t songPosition16th() const noexcept { return songPositionAt(lastPpq_); } // at the next block start
    std::int64_t songPositionAt(double ppq) const noexcept;    // SPP out (ARR-008), any PPQ of this block
    std::uint32_t rowChanges() const noexcept { return rowChanges_; } // rows entered so far (diagnostics, tests)

private:
    bool advance(const BlockContext& ctx, const stages::BlockBuffers::Timing& timing, ScenePatternSwitcher& sw) noexcept;
    bool settle(const BlockContext& ctx, const stages::BlockBuffers::Timing& timing, double tempoBefore,
                double tempoNow, double& tempo) noexcept;
    void cue(ScenePatternSwitcher& sw) noexcept;              // stopped: the cued row's scene/pattern now
    void enterRow(ScenePatternSwitcher& sw, int row, int remaining, double at) noexcept;
    bool needsSwitch(const ScenePatternSwitcher& sw, int row) const noexcept;
    void publish() const noexcept;
    int repeatsOf(int row) const noexcept;
    void resetCue() noexcept;

    const ArrangementPlan* plan_ = nullptr; // the installed plan last seen (owned by the swap table)
    core::ParamId playId_ {}, loopId_ {}, startId_ {};
    bool songMode_ = false, loop_ = false, active_ = false;
    bool playing_ = false;                   // the transport played the last block (startFrom / locate: jump or cue)
    int row_ = 0, remaining_ = 0;            // the playing row and its passes left (current included)
    int cueRow_ = 0, cueRemaining_ = 0;      // what PLAY starts from (0 = the row's repeats)
    int jumpRow_ = -1, jumpRemaining_ = 0;   // startFrom / locate while playing: taken at the next pattern end
    double passStart_ = 0.0;                 // PPQ where the current pass of the row began
    double lastPpq_ = 0.0;                   // the next block's p0 (songPosition16th)
    bool rowBegan_ = false;                  // a row began this block (afterSwitch sets its tempo)
    bool startEdge_ = false;                 // the song started this block (afterSwitch reads the pass start)
    bool passEnded_ = false;                 // a pass end was handled this block
    std::uint32_t rowChanges_ = 0;
};
} // namespace engine::scene
