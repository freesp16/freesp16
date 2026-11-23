// engine::midi::MidiEngine — MIDI in the engine block (spec 02-features/midi.md, sync-and-transport.md
// SYN-010…021, scenes-and-patterns.md SCN-050/051, sequencer.md SEQ-050, NFR-004). A plain member
// of engine::Engine, called directly (steps 2 and 7 of audio-engine.md's block order); audio thread only except the
// set-up calls. Owns the core::midi::MidiHub (the lock-free wire to the port threads and the sender, core/util/midi).
//
//   period start  beginPeriod(): the audio clock anchor (DLL) → the hub (the sender's sample → monotonic mapping)
//   step 1        onParam(): midi.* ParamChanges (core/params/catalogue/midi.cpp)
//   step 2        processIn(): drain the ports, schedule at arrival + the fixed latency (MidiInQueue.h), then per due
//                 event in time order:
//                   F8 clock          → ClockFollower (sync source MIDI CLOCK on the sync port, IN clock enabled):
//                                       every F8 is one tick, never inferred from gaps (ClockFollower.h)
//                   FA / FB / FC / F2 → transport Start / Continue on the sample of the next tick (Transport::startAt:
//                                       quarter 0, or the clock's song position — the ticks it sent before its Stop,
//                                       or the SPP since) / Stop (pause) / Locate (stopped; while playing in pattern
//                                       mode the next tick is there and step 3 jumps), IN START/STOP and SONG POSITION
//                                       enables (SYN-011; ARR-008 in song mode)
//                   notes 0–15        → track n + 1, any channel, frame-accurate trigger (MID-020); Sample tracks only
//                   notes 24–72       → each Sample track on that receive channel, note − 48 semitones, ±12 under
//                                       MASTER TEMPO (MID-021/022); Note Off → Release (GATE tracks) of the tracks
//                                       whose sounding note it started (same port, channel and note)
//                   CC                → the fixed map (CcMap) on every Sample/Thru track of the channel (MID-030/031/
//                                       035), CC 0/32 → queued scene/pattern (SCN-050, ignored while SAMPLING/EDIT is
//                                       open: midi.remoteSelectBlocked, SCN-051), CC 123 → stop those tracks (MID-023)
//                   pitch bend        → RibbonEngine::onPitchBend (PITCH mode only, MID-032)
//                 CC 0 / CC 32 (manual p.88): CC 0 only holds the scene; CC 32 queues (that scene, else the current
//                 one, pattern). Between a CC 0 and its CC 32 the other channel messages are ignored, except those that
//                 only end sound (Note Off, CC 123: nothing hangs); our choice: a CC 0 waits at most kRemoteSelectWait
//                 (1 s), so a lone CC 0 cannot mute MIDI IN, and SAMPLING/EDIT opening or a project open drop it.
//                 applied CCs and the followed tempo go to the UI (UiEvent) for the model / display
//   step 3        the transport calls the follower's correction (Transport::setCorrection): tempo = the follower's,
//                 phase steered onto the clock's ticks by a bounded rate correction (no drift; a jump only when > ½ tick
//                 off). Without a Start / SPP (the unit's own PLAY, sync switched on mid-song, the clock back after a
//                 loss) the transport's position is adopted onto the nearest tick: it keeps its bars and locks.
//   step 7        processOut(): MIDI and AS-1 tracks (MidiTrackPlayer: notes + chords — NOTE2–4 are semitones from
//                 NOTE, an AS-1 track sends NOTE alone — a step's PITCH lock, length, the Active CC1/CC2 values (pitch
//                 bend, aftertouch) before each trigger's notes, bank / program on a scene switch, MID-040…050), the
//                 hanging-note guard (stop, mode change, port change, MID-047), clock / transport / SPP out (ClockOut,
//                 SYN-020/021) → sample-stamped → the hub's out SPSC
//
// Output ports (our choice): MIDI tracks and the clock go to every connected output port whose mode is OUT or OUT +
// MERGE (THRU ports only forward an input); the sender adds the merge / thru bytes.
// Budget: ≤ 3k cycles per block typical (≤ 64 events in, ≤ 64 out), ≤ 8k worst case (bench_midi).
#pragma once

#include "core/util/midi/MidiWire.h"
#include "engine/EngineApi.h"
#include "engine/bridge/ParamBindings.h"
#include "engine/midi/CcMap.h"
#include "engine/midi/ClockFollower.h"
#include "engine/midi/ClockOut.h"
#include "engine/midi/MidiInQueue.h"
#include "engine/midi/MidiOutScheduler.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string_view>

namespace engine::trigger
{
class TriggerRouter;
}
namespace engine::transport
{
class TransportStage;
}
namespace engine::scene
{
class ScenePatternSwitcher;
class SectionSequencer;
}
namespace engine::ribbon
{
class RibbonEngine;
}
namespace engine::sequencer::perf
{
class PerfStage;
}
namespace engine::stages
{
struct BlockBuffers;
}
namespace engine::fx
{
class FxSlotBank;
}

namespace engine::midi
{
// The midi.* parameters (core/params/catalogue/midi.cpp), in kMidiParamIds order.
enum class MidiField : std::uint8_t
{
    InClock, InStartStop, InNoteCc, InSongPosition,
    OutClock, OutStartStop, OutNoteCc, OutSongPosition,
    InChannel, OutChannel,
    SyncSource, SyncPort, ClockLoss, ClockWhileStopped, OutLatency, PortMode, PortOffset, RemoteSelectBlocked,
    Note1, Note2, Note3, Note4, Velocity, Length, NoteBypass,
    PitchBend, PitchBendActive, Aftertouch, AftertouchActive,
    Cc1Num3, Cc1Num4, Cc1Num5, Cc1Num6, Cc1Val3, Cc1Val4, Cc1Val5, Cc1Val6, Cc1Act3, Cc1Act4, Cc1Act5, Cc1Act6,
    Cc1Bypass,
    Cc2Num1, Cc2Num2, Cc2Num3, Cc2Num4, Cc2Num5, Cc2Num6, Cc2Val1, Cc2Val2, Cc2Val3, Cc2Val4, Cc2Val5, Cc2Val6,
    Cc2Act1, Cc2Act2, Cc2Act3, Cc2Act4, Cc2Act5, Cc2Act6, Cc2Bypass,
    BankMsb, BankLsb, Program, Mode,
    Count
};
inline constexpr std::string_view kMidiParamIds[] = {
    "midi.in.clock", "midi.in.startStop", "midi.in.noteCc", "midi.in.songPosition",
    "midi.out.clock", "midi.out.startStop", "midi.out.noteCc", "midi.out.songPosition",
    "midi.in.channel", "midi.out.channel",
    "midi.syncSource", "midi.syncPort", "midi.clockLoss", "midi.clockWhileStopped", "midi.outLatency",
    "midi.portMode", "midi.portOffset", "midi.remoteSelectBlocked",
    "midi.note1", "midi.note2", "midi.note3", "midi.note4", "midi.velocity", "midi.length", "midi.noteBypass",
    "midi.pitchBend", "midi.pitchBendActive", "midi.aftertouch", "midi.aftertouchActive",
    "midi.cc1.cc3", "midi.cc1.cc4", "midi.cc1.cc5", "midi.cc1.cc6",
    "midi.cc1.value3", "midi.cc1.value4", "midi.cc1.value5", "midi.cc1.value6",
    "midi.cc1.active3", "midi.cc1.active4", "midi.cc1.active5", "midi.cc1.active6", "midi.cc1Bypass",
    "midi.cc2.cc1", "midi.cc2.cc2", "midi.cc2.cc3", "midi.cc2.cc4", "midi.cc2.cc5", "midi.cc2.cc6",
    "midi.cc2.value1", "midi.cc2.value2", "midi.cc2.value3", "midi.cc2.value4", "midi.cc2.value5", "midi.cc2.value6",
    "midi.cc2.active1", "midi.cc2.active2", "midi.cc2.active3", "midi.cc2.active4", "midi.cc2.active5",
    "midi.cc2.active6", "midi.cc2Bypass",
    "midi.bankMsb", "midi.bankLsb", "midi.program", "midi.mode",
};
static_assert(std::size(kMidiParamIds) == static_cast<std::size_t>(MidiField::Count));

// One MIDI / AS-1 track's values per [scene][track] (MID-040…050; core::model::MidiTrack).
struct MidiTrackState
{
    std::array<std::int8_t, 4> notes {60, 0, 0, 0}; // NOTE, then NOTE2–4 as semitones from it (0 = off)
    std::uint8_t velocity = 100; // 0 = OFF (step / pad velocity)
    float length = 1.0f;         // steps
    bool noteBypass = false, cc1Bypass = false, cc2Bypass = false; // noteBypass: stored only (NOTE is not bypassable)
    std::int16_t pitchBend = 0;  // −8192…8191
    std::uint8_t aftertouch = 0;
    bool pitchBendActive = false, aftertouchActive = false;
    std::array<std::uint8_t, 10> ccNumber {7, 1, 2, 10, 71, 72, 73, 74, 75, 76}; // CC1 params 3–6, CC2 params 1–6
    std::array<std::uint8_t, 10> ccValue {};
    std::array<bool, 10> ccActive {};
    std::int8_t bankMsb = -1, bankLsb = -1, program = -1;
    std::uint8_t mode = 0; // 0 NOTES, 1 DRUM MAP
};

// The engine objects MIDI drives (engine::Engine fills it at construction; all live as long as the engine).
struct MidiWiring
{
    trigger::TriggerRouter* router = nullptr;
    transport::TransportStage* transport = nullptr;
    scene::ScenePatternSwitcher* switcher = nullptr;
    scene::SectionSequencer* arrangement = nullptr;
    ribbon::RibbonEngine* ribbon = nullptr;
    const sequencer::perf::PerfStage* perf = nullptr;
    const FxCcContext* fx = nullptr;
    ParamSink* sink = nullptr;
    // Pitch-preserving stretch (MASTER TEMPO) on a track of the current scene (MID-021: ±12 then).
    bool (*pitchPreserving)(const void* user, int track) noexcept = nullptr;
    const void* user = nullptr;
};

struct MidiStats // diagnostics (tests, benches, the DIAG line)
{
    std::uint64_t received = 0, late = 0, notesIn = 0, ccsIn = 0, ticksIn = 0, sent = 0, dropped = 0;
};

class MidiEngine
{
public:
    MidiEngine() noexcept;
    ~MidiEngine();
    MidiEngine(const MidiEngine&) = delete;
    MidiEngine& operator= (const MidiEngine&) = delete;

    // ---- message thread (set-up) ----
    void bindParams(const std::function<core::ParamId(std::string_view)>& resolve, bridge::ParamBindings& bindings); // rt-ok: set-up
    void buildFxPages(const fx::FxSlotBank& bank, const std::function<core::ParamId(std::string_view)>& resolve) // rt-ok: set-up
    {
        cc_.build(bank, resolve);
    }
    void attach(const MidiWiring& w) noexcept { w_ = w; }
    void prepare(double sampleRate) noexcept;
    core::midi::MidiHub& hub() noexcept { return hub_; }
    const core::midi::MidiHub& hub() const noexcept { return hub_; }

    // ---- audio thread ----
    void beginPeriod(std::int64_t firstFrame, std::int64_t hostNs, int frames) noexcept;
    bool onParam(const ParamChange&) noexcept; // a midi.* message (consumed) or false
    void processIn(BlockContext& ctx) noexcept;                                   // step 2
    void processOut(const BlockContext& ctx, const stages::BlockBuffers& b) noexcept; // step 7
    // The tempo the playhead shows: the follower's display tempo while following an external clock, else `bpm`.
    double displayTempo(double bpm) const noexcept { return following_ && follower_.displayTempo() > 0.0 ? follower_.displayTempo() : bpm; }

    // ---- inspection (tests, benches; audio thread or after the device stopped) ----
    const ClockFollower& follower() const noexcept { return follower_; }
    ClockFollower& follower() noexcept { return follower_; }
    const MidiOutScheduler& out() const noexcept { return out_; }
    const AudioClock& clock() const noexcept { return clock_; }
    bool following() const noexcept { return following_; }
    bool clockLost() const noexcept { return following_ && follower_.lost(blockStart_); } // SYN-013 (or no clock yet)
    int inputLatency() const noexcept { return MidiInScheduler::latencyFor(clock_.periodFrames()); }
    const MidiTrackState& track(int scene, int t) const noexcept { return tracks_[scene & 15][t & 15]; }
    int receiveChannel(int track) const noexcept { return rxChannel_[static_cast<std::size_t>(track & 15)]; }
    MidiStats stats() const noexcept;
    std::uint32_t outputPorts() const noexcept; // where MIDI tracks / clock go this block

private:
    static void correction(void* self, double ppq, double ppqPerFrame, double& bpm, double& phase) noexcept;
    void correct(double ppq, double ppqPerFrame, double& bpm, double& phase) noexcept;
    void handle(const DueEvent& d, BlockContext& ctx) noexcept;
    void channelMessage(const MidiEvent& e, int frame) noexcept;
    void controlChange(int ch, int cc, int value, int frame) noexcept;
    void select(int scene, int pattern) noexcept;
    core::midi::CcKind ccKind(int cc) const noexcept;
    void trackOut(const BlockContext& ctx, const stages::BlockBuffers& b, std::uint32_t ports) noexcept;
    int currentScene() const noexcept;
    bool trackIsSample(int track) const noexcept;
    bool trackIs(int track, int mode) const noexcept;
    void updateFollowing() noexcept;

    core::midi::MidiHub hub_;
    MidiWiring w_ {};
    double sampleRate_ = 44100.0;
    AudioClock clock_;
    MidiInScheduler in_;
    ClockFollower follower_;
    CcMap cc_;
    MidiOutScheduler out_;
    ClockOut clockOut_;
    bool hubClaimed_ = false;

    // Parameter routing: ParamId.value → MidiField (Count = not ours).
    std::array<std::uint8_t, bridge::ParamBindings::kMaxParams> field_ {};
    core::ParamId sceneQueue_ {};

    // Settings (midi.*).
    // = the ParamSpec / model defaults (utility.md: every enable ENABLE). postMidiSettings posts every enable and
    // channel on a project open, and the MIDI track values after a reset (core::midi::kProjectOpenScope).
    std::array<bool, 4> inEnable_ {true, true, true, true}, outEnable_ {true, true, true, true};
    std::array<std::int8_t, 16> rxChannel_ {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}; // 0 = OFF
    std::array<std::int8_t, 16> txChannel_ {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    int syncSource_ = 0, syncPort_ = 0, clockLoss_ = 0;
    bool clockWhileStopped_ = false, remoteBlocked_ = false;
    std::array<std::int8_t, core::midi::kMaxPorts> portMode_ {};
    MidiTrackState tracks_[16][16]; // [scene][track], default-initialised (no braces: GCC 11 build time)

    // Runtime state.
    bool following_ = false, correctionInstalled_ = false;
    enum class Pending : std::uint8_t { None, Start, Continue } pendingStart_ = Pending::None;
    double startPpq_ = 0.0;   // where a pending Start / Continue starts (its first tick)
    double resumePpq_ = -1.0; // the clock's song position since its Stop / SPP (a Continue resumes there); −1 = none
    bool lostHandled_ = false;
    std::int64_t blockStart_ = 0;
    std::uint16_t midiMask_ = 0;
    std::array<std::uint32_t, core::midi::kMaxPorts> generation_ {};
    std::array<std::int16_t, 16> heldNote_{}; // per track: port << 11 | channel << 7 | note that started it, −1 none
    int remoteScene_ = -1;                    // a CC 0's scene 0–15 waiting for its CC 32, −1 = none
    std::int64_t remoteSceneAt_ = 0;          // the CC 0's engine frame
    double shownTempo_ = 0.0;
    std::uint32_t activity_ = 0;
    std::uint64_t notesIn_ = 0, ccsIn_ = 0, ticksIn_ = 0;
};
} // namespace engine::midi
