// The composition root's half of the P3 extensions (E11 Toggle, E4 MIDI learn, E8 export for the reference, E12
// strip moves recorded). Message thread. With every ext.* flag off (the default) nothing here posts, publishes,
// writes or logs — the app behaves as without the extensions.
//
//   E11  the model's per-track TOGGLE (Track::extras "native.ext.trk.toggle") → ParamChange ext.trk.toggle, on project
//        open, on edits and when the flag turns on (0 again when it turns off) → engine/trigger/ext/toggle
//   E4   while ext.midi.learn is on: the CC overlay on the live MIDI hub (services/midilearn: every CC the fixed map
//        leaves free reported to the UI), learn capture, the project's bindings applied through the model path of the
//        fixed CCs (setRemote: non-undoable, like an incoming fixed CC); bindings saved in Project::extras (undoable)
//   E8   `cmd ext-export <new folder> [name]`: the open project exported for the reference (services/interop/
//        sp16export), samples located through the session's resolver
//   E12  every UI frame while ext.strip.record is on: the strip's live state + the playhead → SetLock commands
//        (ui/input/ext/striprecord); where the strip acts follows ribbon.applyTo (held pads' tracks by default)
//
// Scripted diagnostics (`cmd ext-…`, logged "EXT …"): ext-flag <flag> on|off, ext-state, ext-toggle <track 1-16>
// on|off, ext-learn <param id> [track 1-16] [lo hi], ext-unlearn <channel> <cc>, ext-export <absolute folder> [name].
// The intended UI slots (TRIGGER MODE's TOGGLE value, UTILITY "MIDI LEARN", PROJECT rail item 4, TOUCH STRIP
// SETTING rail item 2) are not reachable without editing other packages' screens: they are reported, not built.
#pragma once

#include "core/commands/Command.h"
#include "core/model/Project.h"
#include "core/util/midi/CcTable.h"
#include "core/util/midi/MidiWire.h"
#include "engine/EngineApi.h"
#include "io/controllers/ControlSurface.h"
#include "services/midilearn/MidiLearn.h"
#include "ui/input/ext/striprecord/StripRecorder.h"

#include <juce_core/juce_core.h>

#include <array>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace app::ext
{
struct ExtEnv
{
    core::CommandBus* bus = nullptr;
    engine::EngineBridge* bridge = nullptr;
    std::function<void(const std::string&)> log;
    std::function<void(const core::ParamSpec&, std::uint16_t scope, float value)> setRemote; // model path of a CC
    std::function<float(const core::ParamSpec&, std::uint16_t scope)> value;
    std::function<int()> selectedTrack;   // 0–15
    std::function<bool()> recArmed;       // REC armed (UiState::recArmed)
    std::function<int()> padMode;         // ui::input::pads::PadMode (0 PLAY, 1 TRACK, 2 MUTE, 3 SLICE, 4 SCALE)
    std::function<std::optional<juce::File>(const std::string&)> resolveSample;
    // The preferences (Preferences::get): the source of the ext.* flags (an environment value still wins,
    // tests and the smoke check); null = the environment only.
    std::function<float(std::string_view key)> preference;
};

class Extensions
{
public:
    explicit Extensions(ExtEnv env);
    ~Extensions(); // the fixed CC table back on the hub
    Extensions(const Extensions&) = delete;
    Extensions& operator= (const Extensions&) = delete;

    void onChange(const core::ChangeSet&, const core::model::Project&); // the bus's bridge hook
    void onProjectReplaced(const core::model::Project&);
    bool onMidiUi(const core::midi::UiEvent&); // true = an E4 carrier event (consumed)
    void handle(const io::ControlEvent&);       // held pads (E12)
    void frame(double nowMs, const engine::PlayheadFeed&, int stripMode, bool stripContact, int stripPosition);
    bool command(const std::string& cmd, std::istringstream& in); // `cmd ext-*`; false = not an ext command

private:
    bool hasProject() const;
    const core::model::Project& project() const;
    void log(const std::string& line) const;
    void syncToggles(const core::model::Project&);
    void syncLearn(const core::model::Project&);
    void publishOverlay(bool on);
    std::uint16_t stripTracks() const;
    void storeLearnMap();

    ExtEnv env_;
    bool toggleOn_ = false, learnOn_ = false;
    std::array<std::uint16_t, 16> postedToggles_ {}; // [scene] bit t = ext.trk.toggle = 1 posted
    services::midilearn::LearnMap learn_;
    services::midilearn::LearnSession session_;
    const core::midi::CcTable* fixed_ = nullptr; // the hub's table before the overlay
    std::uint64_t learnApplied_ = 0;
    ui::input::ext::StripRecorder strip_;
    std::vector<ui::input::ext::LockWrite> writes_;
    std::uint16_t heldPads_ = 0, heldTracks_ = 0;
    std::uint64_t stripWrites_ = 0;
};
} // namespace app::ext
