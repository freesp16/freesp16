// The transport keys on the message thread (spec 02-features/sync-and-transport.md
// SYN-001/003/005/006, sequencer.md SEQ-006/044, 03-ui/control-surface.md UI-102 extension controls, UI-150 lamps):
//
//   PLAY   Stopped → Play (with the pre-count bars of transport.preCount when REC is armed, SEQ-044);
//          Playing → Stop(0) (pause, the position is kept); Paused → Continue; during the pre-count → Stop(1)
//          Under MIDI clock with IN START/STOP/CONTINUE enabled (the feed's externalStart) only the external device
//          starts the sequencer (its Start / Continue): PLAY while stopped or paused toggles the PLAY arm (lamp
//          blinking: waiting for the clock's Start) and posts nothing; PLAY while playing does nothing. The arm ends
//          when the transport starts, on STOP, or when the sync (or IN START/STOP/CONTINUE) is switched off. (The
//          engine drops a UI Play / Continue then, too.) With IN START/STOP/CONTINUE disabled the master cannot start
//          the unit (manual p.45), so PLAY works as without sync and the engine locks the transport onto the clock.
//   STOP   Playing / Paused / pre-count → Stop(1) (back to step 1, tails ring); Stopped → Stop(2) (hard stop:
//          everything silent, tails included)
//   REC    toggles the record arm (the sequencer's recorder reads recArmed())
//   TAP    (ext.tap, and the BPM/QUANTIZE panel's TAP) core::musictime::TapTempo → transport.tempo
//   NUDGE  (ext.nudgeDown / ext.nudgeUp, and the panel's ◀ ▶) transport.nudge −1 / +1 while held, 0 on release
//
// Values go through the shell's ParamAccess when one is given (model, undo, the panel's display), else straight to the
// engine (ParamChange / TransportCmd SetTempo). The controller keeps its own view of the transport state and follows
// the engine's PlayheadFeed once per frame (sync), so a start/stop sent by someone else (project load, MIDI) is seen.
// publish() writes the shell's UiState: PLAY lamp / status (playing), REC (recArmed), the context bar's BPM text and
// BAR:BEAT:STEP (SYN-006), and the sync badge. Under MIDI clock (the feed's externalClock, SYN-010/012) the BPM is the
// followed clock's, the badge reads MIDI (in the warning colour while no tick arrives, SYN-013), and TAP / NUDGE do
// nothing (the BPM/QUANTIZE panel greys them out).
#pragma once

#include "core/params/ParamRegistry.h"
#include "core/util/musictime/MusicTime.h"
#include "engine/EngineApi.h"
#include "io/controllers/ControlSurface.h"

#include <cstdint>

namespace ui
{
class ParamAccess;
struct UiState;
} // namespace ui

namespace ui::input::transport
{
enum class State : std::uint8_t
{
    Stopped,
    Playing,
    Paused,
    PreCount
};

class TransportController
{
public:
    // Blocks the engine needs to report a command back in its feed before sync() trusts the feed again (≈ 46 ms).
    static constexpr std::uint64_t kSettleBlocks = 32;

    explicit TransportController(engine::EngineBridge&, ParamAccess* values = nullptr,
                                 const core::ParamRegistry& params = core::builtinParams());

    void onPlay();
    void onStop();
    void onRec();
    void onTap(double nowMs);
    void onNudge(int direction, bool held);
    // Convenience for the input dispatcher: PLAY / STOP / REC / ext.tap presses, ext.nudgeDown / ext.nudgeUp press and
    // release; true when the event was consumed.
    bool handle(const io::ControlEvent&);
    // Once per UI frame with the engine's playhead.
    void sync(const engine::PlayheadFeed&);
    // The shell's state: playing, recArmed, bpm text, position (pattern-relative when patternStartPpq is given).
    void publish(UiState&, double patternStartPpq = 0.0) const;

    State state() const noexcept { return state_; }
    bool playing() const noexcept { return state_ == State::Playing; }
    bool recArmed() const noexcept { return recArmed_; }
    bool playArmed() const noexcept { return playArmed_; } // under MIDI clock: PLAY pressed, waiting for its Start
    void setRecArmed(bool armed) noexcept { recArmed_ = armed; }
    int nudge() const noexcept { return nudge_; }
    int preCountBars() const; // transport.preCount (0–2), 0 without a ParamAccess
    double tempo() const;     // the followed clock's BPM under MIDI clock, else transport.tempo, else the feed's BPM
    bool externalClock() const noexcept { return external_; }
    const core::musictime::TapTempo& tapTempo() const noexcept { return tap_; }

private:
    void post(const engine::TransportCmd&);
    void setParam(const core::ParamSpec*, float value);

    engine::EngineBridge& bridge_;
    ParamAccess* values_;
    const core::ParamRegistry& params_;
    const core::ParamSpec *tempo_ = nullptr, *nudgeSpec_ = nullptr, *preCount_ = nullptr;
    core::musictime::TapTempo tap_;
    State state_ = State::Stopped;
    bool recArmed_ = false, playArmed_ = false;
    int nudge_ = 0;
    std::uint64_t feedBlock_ = 0, postedAt_ = 0;
    bool posted_ = false;
    double feedPpq_ = 0.0, feedBpm_ = 120.0;
    bool external_ = false, clockLost_ = false; // the feed: following MIDI clock, no tick lately
    bool externalStart_ = false;                // the feed: only the clock's Start starts the transport
};
} // namespace ui::input::transport
