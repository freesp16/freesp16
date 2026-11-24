// The unit's MIDI port preferences across the port-order change (io/midi/MidiService.h "Slots").
//
// The unit's two ports used to take their slots in ALSA card order: on a normal start (USB-B in MIDI mode) the USB
// gadget's card came first, so USB was port 1 and DIN port 2. The DIN port is now always port 1 and USB port 2. The
// preferences saved per port under the old order — SYNC PORT (midi.syncPort 1 / 2), OUT MODE, CLOCK OFFSET and IN of
// ports 1 and 2 — are swapped once, so a SYNC SOURCE of MIDI CLOCK (MIDI DIN) still follows the DIN socket. The OUT
// MODE values name an input (MERGE / THRU A or B), and the inputs swapped too: every port's mode maps A ↔ B, and the
// USB port keeps only what it can do (core::midi::legalPortMode). The marker midi.portLayout = 1 records it; a new
// install only gets the marker. The unit only (the composition root calls it before the preferences are applied): the
// desktop's port order did not change.
#pragma once

#include "core/params/ParamRegistry.h"
#include "services/settings/Preferences.h"

namespace services::settings
{
inline constexpr std::string_view kMidiPortLayoutKey = "midi.portLayout";

// True when it changed anything.
bool migrateMidiPortLayout(Preferences&, const core::ParamRegistry& = core::builtinParams());
} // namespace services::settings
