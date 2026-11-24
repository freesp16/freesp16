// The MIDI OUTPUT LATENCY preference across the change of its meaning (core/params/catalogue/midi.cpp).
//
// midi.outLatency used to be the audio output latency itself (default 12 ms, the output buffer): the sender added it
// so the clock and notes left with their audio. The sender now places every message when its frame is heard on its
// own (the clock anchor's lead, io/midi/MidiSender.h), and midi.outLatency is an extra trim, default 0. A stored 12
// — the old default, written when a default was set explicitly — would now delay everything by the buffer a second
// time, so it is reset to the default once; any other stored value was the user's own choice and is kept. The marker
// midi.outLatencyTrim = 1 records it; a new install only gets the marker. The composition root calls it before the
// preferences are applied, on the unit and the desktop alike.
#pragma once

#include "core/params/ParamRegistry.h"
#include "services/settings/Preferences.h"

namespace services::settings
{
inline constexpr std::string_view kMidiOutLatencyKey = "midi.outLatencyTrim";

// True when it reset a stored 12.
bool migrateMidiOutLatency(Preferences&, const core::ParamRegistry& = core::builtinParams());
} // namespace services::settings
