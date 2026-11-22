// Track commands (spec 02-features/tracks.md TRK-001…032, pads-and-scales.md PAD-006;
// state-and-undo.md): the undoable edits behind TRACK MENU (TRACK TYPE), TRACK SETTING (CHOKE, PAD COLOR, VELOCITY),
// the HOME TRIGGER MODE knob and the MUTE pad mode. Each is a SetParam on the model field (one undo step, not
// coalesced: a toggle is its own step), whose model path is the ParamSpec's ("scene[s].track[t].<field>", ParamPath
// grammar), so the change reaches observers and — through forEachTrackChange — the engine as a ParamChange.
//
// Message thread only (they allocate).
#pragma once

#include "core/commands/Command.h"
#include "core/commands/CommandGroup.h"
#include "core/model/enums/Enums.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace core::model
{
struct Project;
}

namespace core::commands::track
{
std::unique_ptr<Command> setMute(int scene, int track, bool mute);                    // "track.mute"
std::unique_ptr<Command> setMode(int scene, int track, model::TrackMode mode);        // "track.mode", TRK-002
std::unique_ptr<Command> setTriggerMode(int scene, int track, model::TriggerMode m); // "track.triggerMode"
std::unique_ptr<Command> setChokeGroup(int scene, int track, int group);              // "track.chokeGroup" 0–8
std::unique_ptr<Command> setPadColour(int scene, int track, int colour);              // "track.padColor" 0–15
std::unique_ptr<Command> setPadVelocity(int scene, int track, bool velocitySensitive); // "track.padVelocity"

// PAD-006: SHIFT + pad n in MUTE mode. When the scene is not already "every track but n muted", mutes every other
// track and unmutes n; when it is, unmutes all 16. One CommandGroup = one undo step ("MUTE"); only tracks whose
// state changes get a child. nullptr when nothing would change.
std::unique_ptr<CommandGroup> muteOthers(const model::Project&, int scene, int track);
// The plain MUTE-mode toggle of track n (PAD-006).
std::unique_ptr<Command> toggleMute(const model::Project&, int scene, int track);
// Hold MUTE + pad n: track n's SOLO, the MIXER's `track.solo` field (forwarded to the engine by
// ui::forwardModelChanges, not by forEachTrackChange).
std::unique_ptr<Command> setSolo(int scene, int track, bool solo);
std::unique_ptr<Command> toggleSolo(const model::Project&, int scene, int track);

// The track fields these commands write, as (ParamSpec id, value in engine wire units) for every path of `changes`
// that names one; `emit(scene, track, paramId, value)`. The composition root's CommandBus bridge hook (or the pad
// controller) turns them into engine::ParamChange.
void forEachTrackChange(const ChangeSet& changes, const model::Project&,
                        const std::function<void(int scene, int track, std::string_view paramId, float value)>& emit);
} // namespace core::commands::track
