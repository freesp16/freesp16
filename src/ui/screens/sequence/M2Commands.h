// Local commands of the ui::m2 screens (see M2Session.h): the step edits are core::commands::sequencer's (toggleStep
// below only forwards); still local: stand-ins for core::commands::scene (replaceScene, replacePattern, clearScene,
// clearPattern, setSceneName / Colour / Bpm), plus the track copy of the TRACK tab. Each one replaces a whole pattern,
// scene or track with a version computed from the project it is created against, so the inverse is the exact previous
// value (undo restores it bit-identically). Indices are 0-based. Message thread only.
#pragma once

#include "core/commands/Command.h"
#include "core/model/Project.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ui::m2::cmd
{
using core::model::Project;

// SEQ-040: forwards to core::commands::sequencer::toggleStep (kept for the ui::m2 fixtures; the project
// argument is unused).
std::unique_ptr<core::Command> toggleStep(const Project&, int scene, int pattern, int track, int step,
                                          core::model::TriggerType type, int velocity);
// Forwards to core::commands::sequencer::setLocks (kept for the ui::m2 fixtures; the project argument is unused).
std::unique_ptr<core::Command> setLocks(const Project&, int scene, int pattern, int track, std::span<const int> steps,
                                        core::model::LockSlot slot, std::span<const float> values, float removeAt,
                                        std::string_view label);

// SCENE MANAGER SEQ. tab: copies steps [srcFirst, srcFirst + count) of the tracks in `trackMask` from `source` onto
// [first, first + count) of scene/pattern (with their locks); a null `source` clears that range instead.
std::unique_ptr<core::Command> pasteSteps(const Project&, int scene, int pattern, std::uint16_t trackMask, int first,
                                          int count, const core::model::Pattern* source, int srcFirst);

// SCENE MANAGER (UI-285…288): forwards to core::commands::scene (kept for the ui::m2 call sites and tests).
std::unique_ptr<core::Command> replaceScene(int dst, const core::model::Scene&);
std::unique_ptr<core::Command> replacePattern(int scene, int dst, const core::model::Pattern&);
std::unique_ptr<core::Command> clearScene(const Project&, int scene);
std::unique_ptr<core::Command> clearPattern(int scene, int pattern);
std::unique_ptr<core::Command> setSceneName(int scene, std::string name);
std::unique_ptr<core::Command> setSceneColour(int scene, int colour);
std::unique_ptr<core::Command> setSceneBpm(int scene, std::optional<double> bpm);
std::unique_ptr<core::Command> setPatternLength(const Project&, int scene, int pattern, int steps);
// TRACK tab (UI-288): the track's settings from `source`, and when `sequenceFrom` is set the column `sourceTrack` of
// its 16 patterns (with locks) into the column of `track`. resetTrack: defaults (EDT-009), with the sequences when
// asked.
std::unique_ptr<core::Command> replaceTrack(const Project&, int scene, int track, const core::model::Track& source,
                                            const core::model::Scene* sequenceFrom, int sourceTrack);
std::unique_ptr<core::Command> resetTrack(const Project&, int scene, int track, bool withSequence);
// TRACK SETTING exchanges both tracks and their columns in every pattern of the scene.
void exchangeTrackSlots(int scene, int source, int destination);
} // namespace ui::m2::cmd
