// Builds the fixed incoming CC map (core/util/midi/CcTable.h) from the parameter registry on the message thread
// (spec 02-features/midi.md MID-030/031/035, scenes-and-patterns.md SCN-050, effects/parameters.md FX-086):
// every ParamSpec with a midiCc (ParamRegistry::byMidiCc) becomes Linear (numeric) or Bands (enum / bool); CC 0 / 32
// select scene / pattern, CC 46–51 / 63–68 address FX1 / FX2 page positions, CC 123 is All Notes Off; the FX knob
// grids (free, and the sync grid of every `.div` form) come from core::fx (core/params/catalogue/fx.h).
#pragma once

#include "core/params/ParamRegistry.h"
#include "core/util/midi/CcTable.h"

namespace io::midi
{
void buildCcTable(const core::ParamRegistry& registry, core::midi::CcTable& out);
} // namespace io::midi
