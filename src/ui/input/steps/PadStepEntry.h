// Step input in SLICE and SCALE pad mode (the manual's 8.4 "Programing triggers when performing in SLICE mode", p.44:
// hit a pad, then press the step key; or hit a pad while the step key is held — "in SCALE mode in a similar way";
// 1.6 "Selection of PITCH/NOTE", p.28; SLC-005). The app's document calls these with the selected track and the
// pad it last played; `step1to16` is a step key of the shown bar (StepController::patternStep). Message thread.
//
//   step entry    a step key release that entered a trigger (StepController::onStepKey true): the new trigger plays
//                 the last slice (its slice-number lock) / the selected SCALE key (its PITCH lock, none for the root).
//                 A release that removed a trigger, ended a hold + turn or a COPY / PASTE gesture programs nothing
//   held step     a SCALE pad hit while a step key is held: an empty step gets a trigger at the pad's velocity, and
//                 the step plays the key exactly as step entry would program it (the root: no PITCH lock, an earlier
//                 key's lock goes); the held key's release then toggles nothing
//   MIDI tracks   no SLICE mode (manual p.49, "SLICE mode cannot be used for MIDI tracks"; AS-1 tracks are MIDI too);
//                 a SCALE key's PITCH lock transposes the track's notes (engine::midi), so it keeps the live
//                 path's ±127 semitones (engine::trigger::TriggerRouter) instead of the sample PITCH range ±24
#pragma once

#include "core/commands/Command.h"
#include "core/model/enums/Enums.h"
#include "core/params/ParamSpec.h"

namespace ui::input::steps
{
class StepController;

constexpr int kSliceLockSlot = 22; // the slice-number lock (SLC-005)
constexpr int kPitchLockSlot = 4;  // PITCH (playback.pitch), semitones
constexpr float kMidiPitchLockRange = 127.0f;

// A MIDI or TORAIZ AS-1 track: it plays MIDI notes, no sample.
constexpr bool isMidiMode(core::model::TrackMode m) noexcept
{
    return m == core::model::TrackMode::Midi || m == core::model::TrackMode::Instrument;
}
// A PITCH lock of `semitones` kept in range for a track of mode `m`: `pitch`'s range (±24) on a Sample track,
// ±kMidiPitchLockRange on a MIDI / AS-1 track (the notes the engine cannot play, outside 0–127, are skipped there).
float clampPitchLock(const core::ParamSpec& pitch, core::model::TrackMode m, float semitones) noexcept;

// A step key release that entered a trigger in SCALE pad mode: the trigger gets `pitchLock` (NaN = the root key or
// no key: nothing to program).
void scaleStepEntry(core::CommandBus&, const StepController&, int track, float pitchLock, int step1to16);
// A SCALE pad hit at `velocity` while `step1to16` is held: see the header comment (NaN = the root key).
void scaleStepHeld(core::CommandBus&, StepController&, int track, float pitchLock, int velocity, int step1to16);
// A step key release that entered a trigger in SLICE pad mode: the trigger plays `slice` (0-based; < 0 = no slice
// played yet). Nothing on a MIDI / AS-1 track.
void sliceStepEntry(core::CommandBus&, const StepController&, int track, int slice, int step1to16);
} // namespace ui::input::steps
