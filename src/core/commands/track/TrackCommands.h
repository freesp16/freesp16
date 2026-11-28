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
#include "core/model/Track.h"
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
std::unique_ptr<Command> setPlugin(int scene, int track, model::PluginTrack plugin);   // "track.plugin"

// Per-pattern mute: the user's mutes (MUTE pads, MIXER) live in the current pattern's `trackMutes`
// ("scene[s].pattern[p].mutes", saved with the pattern), so every pattern keeps its own set and the engine swaps it
// at the pattern switch (SCN-012). `track.mute` stays the track-level mute (imports); it mutes in every pattern.
// isMuted = either of the two, for the current pattern (settings.currentPattern).
bool isMuted(const model::Project&, int scene, int track) noexcept;
std::unique_ptr<Command> setPatternMute(int scene, int pattern, int track, bool mute); // one pattern's bit
// Mutes / unmutes track n in the current pattern; nullptr when isMuted already matches. Unmuting a track-level mute
// clears it and mutes the track in the scene's other patterns instead (one "MUTE" group), so only this one unmutes.
std::unique_ptr<Command> setMuted(const model::Project&, int scene, int track, bool mute);

// PAD-006: SHIFT + pad n in MUTE mode. When the current pattern is not already "every track but n muted", mutes
// every other track and unmutes n; when it is, unmutes all 16. One CommandGroup = one undo step ("MUTE"); only tracks
// whose state changes get a child. nullptr when nothing would change.
std::unique_ptr<CommandGroup> muteOthers(const model::Project&, int scene, int track);
// The plain MUTE-mode toggle of track n in the current pattern (PAD-006); nullptr never (the state always flips).
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
