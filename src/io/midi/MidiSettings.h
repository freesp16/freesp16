// The project's MIDI values → the engine (spec 02-features/midi.md MID-010/011/012/040…050). Message
// thread. For the composition root's project-open path (app/Document.cpp forwardAll, like
// io::postRibbonSettings): the MidiSettings enables and channels (project.midi) and every scene's MIDI track values
// (Scene.track[t].midi). The 8 enables and 32 channels are always posted (the engine holds the previous project's);
// the MIDI track values are reset in the engine first (core::midi::kProjectOpenScope), then only values that differ
// from the defaults (the ParamSpec defaults) are posted, so a default project costs 41 messages instead of 12 000.
// Returns false when the bridge refused a message.
//
// Incoming CCs flow back the other way: drainUiEvents() hands the engine's UiEvents (core/util/midi/MidiWire.h:
// applied CCs, the followed tempo, activity) to a callback on the message thread; the model write (a non-undoable,
// coalesced SetParam) belongs to whoever owns the document (open: app/Document.cpp does not write it yet; until then
// the engine holds the CC value and the model keeps the old one).
#pragma once

#include "core/model/Project.h"
#include "core/params/ParamRegistry.h"
#include "core/util/midi/MidiWire.h"
#include "engine/EngineApi.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace io::midi
{
bool postMidiSettings(const core::model::Project&, engine::EngineBridge&, const core::ParamRegistry&);

// The app's settings (platform::Settings: the settings file's key=value lines + <PREFIX>_<KEY> environment
// overrides), handed over by the composition root before the control sources are created. The MIDI source reads
// midi, midi_providers, midi_virtual, midi_virtual_out, midi_spin_us from here, the ALSA provider midi_usb; an
// environment variable <PREFIX>_<KEY> still wins (the app's rule), so tests and the smoke check need no settings file.
using SettingsMap = std::map<std::string, std::string, std::less<>>;
void configure(const SettingsMap& settings);
std::string setting(std::string_view key, std::string_view fallback = {});

// UTILITY › MIDI PORTS, SYNC SOURCE: the port names by slot 0–7 ("" = no port; the output's name,
// else the input's), published by the MIDI source after each hot-plug pass; message thread.
std::vector<std::string> portNames();
void publishPortNames(std::vector<std::string> names);
// MIDI PORTS › IN (preference midi.portIn#p, MID-002): an input slot that is off is read but its messages are dropped
// (no engine, no thru). Any thread (relaxed atomics; the input threads read it per message).
void setInputEnabled(int slot, bool on) noexcept;
bool inputEnabled(int slot) noexcept;

// The UI side of the hub (the MIDI control source calls it once per frame).
using UiEventSink = std::function<void(const core::midi::UiEvent&)>;
void setUiEventSink(UiEventSink sink); // message thread; null = events are only counted
int drainUiEvents(core::midi::MidiHub& hub, int max = 256);
double lastFollowedTempo() noexcept;  // the latest Tempo event (0 = none)
} // namespace io::midi
