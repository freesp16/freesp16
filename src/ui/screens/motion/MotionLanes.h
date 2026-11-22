// What TRACK MENU's MOTION bar and the MOTION page read of the recorded motion: the locks (p-locks, one per step and
// lock slot) of the selected track in the current pattern, grouped per lock slot into lanes.
//   specOf      the ParamSpec a lock slot drives for the selected track: the track slots 0–22 by their ids
//               (core::commands::sequencer::lockSlotOf), FX1 / FX2 slots 23–34 by the effect the FX slot holds now
//   scopeOf     the value scope of that spec (the base value an unlocked step plays)
//   pattern     the current pattern of the document (scene / pattern of UiState), null without a project
//   lanes       bit s = slot s holds a lock on one of the pattern's steps (selected track)
// Message thread; no allocation.
#pragma once

#include "core/model/Pattern.h"
#include "core/params/ParamSpec.h"

#include <cstdint>

namespace ui
{
class ShellApi;
}

namespace ui::motion
{
const core::ParamSpec* specOf(ShellApi&, int slot);
std::uint16_t scopeOf(ShellApi&, const core::ParamSpec&);
const core::model::Pattern* pattern(ShellApi&);
int patternLength(ShellApi&);

std::uint64_t recordedSlots(const core::model::Pattern&, int track0); // slots locked on any step
int stepsWithMotion(const core::model::Pattern&, int track0, int length);
std::uint64_t lanes(ShellApi&);
// The value `spec` takes at height `y01` (0 = bottom, 1 = top) of a lane: clamped, integral for integer and enum types.
float valueAt(const core::ParamSpec&, float y01) noexcept;
} // namespace ui::motion
