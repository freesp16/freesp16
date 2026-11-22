// Scene and pattern commands (spec 02-features/scenes-and-patterns.md SCN-003/011/020…027/040…044,
// 01-architecture/state-and-undo.md). Indices are 0-based. Message thread.
//
// Edits (undoable, EDT-U1): name / colour / BPM of a scene and a pattern's length are SetParam value commands (they
// coalesce, EDT-U2); replace / clear of a scene or a pattern keep the previous value as their inverse, so undo restores
// it bit-identically. Only the addressed scene or pattern is touched (SCN-002: scene 3 edits never reach scene 4).
//
// Switching (not undoable, never makes the project dirty): selectScenePattern reports the ChangeSet path
// queuePath(value) ("queue[37]") and leaves the model alone — the engine decides when the switch happens (SCN-020); the
// bridge side (ui::input::scenes::SceneSync) posts it as ParamChange{scene.queue, 0xFFFF, value}. When the engine reports
// SwitchDone, setCurrentScenePattern records the new current scene/pattern in the model (settings.current*).
#pragma once

#include "core/commands/Command.h"
#include "core/model/Project.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace core::commands::scene
{
// scene.queue values (same numbers as engine::scene::kQueue*): s × 16 + p, + kQueueApplyBpm = apply the scene BPM at
// this switch (the ASK popup answered yes, SCN-005); kQueueCancel cancels (SCN-021); block / unblock = SCN-023.
inline constexpr int kQueueCancel = -1, kQueueBlock = -2, kQueueUnblock = -3, kQueueApplyBpm = 256;

std::string queuePath(int value);                          // "queue[<value>]"
std::optional<int> queueValueOf(std::string_view path) noexcept; // the value of a queuePath, else nullopt

// ---- switching (not undoable) ----
std::unique_ptr<core::Command> selectScenePattern(int scene, int pattern);                // queued (SCN-020/024)
std::unique_ptr<core::Command> selectScenePattern(int scene, int pattern, bool applyBpm); // ASK answered (SCN-005)
std::unique_ptr<core::Command> cancelScenePattern();                                      // SCN-021
std::unique_ptr<core::Command> setSwitchable(bool);                                       // SCN-023
// The model's current scene/pattern (settings.currentScene/currentPattern), after the engine's SwitchDone.
std::unique_ptr<core::Command> setCurrentScenePattern(int scene, int pattern);

// ---- edits (undoable) ----
std::unique_ptr<core::Command> setPatternLength(int scene, int pattern, int steps); // clamped 1–64 (UI: 8–64, SCN-011)
// The length change the UI makes (after hardware feedback): a pattern made longer repeats its content into the new
// steps (new step s = old step s mod the old whole bars — the old length under one bar —, every track, p-locks
// included), so 1 → 2 bars duplicates bar 1, 2 → 4 bars plays 1-2-1-2 and growing one step at a time still copies
// bar by bar. Only when the new steps are all empty (steps kept from before a shortening come back
// as they were) and the old ones hold something; otherwise, and for a shorter length, setPatternLength. One undo step.
std::unique_ptr<core::Command> resizePattern(const model::Project&, int scene, int pattern, int steps);
std::unique_ptr<core::Command> setSceneName(int scene, std::string);
std::unique_ptr<core::Command> setSceneColour(int scene, int colour);              // palette index 0–15 (SCN-003)
std::unique_ptr<core::Command> setSceneBpm(int scene, std::optional<double>);      // nullopt = none (SCN-004)
// SCENE MANAGER (SCN-040, UI-285…288).
std::unique_ptr<core::Command> replaceScene(int dst, const model::Scene&);
std::unique_ptr<core::Command> replacePattern(int scene, int dst, const model::Pattern&);
std::unique_ptr<core::Command> clearScene(int);                                    // model::resetScene (defaults)
std::unique_ptr<core::Command> clearPattern(int scene, int pattern);               // model::defaultPattern()

// ---- views ----
bool patternHasContent(const model::Pattern&) noexcept;    // any trigger (SCN-026)
std::uint16_t patternContentMask(const model::Scene&) noexcept; // bit p = pattern p has content
bool sceneHasContent(const model::Scene&) noexcept;        // at least one pattern with content (SCN-027, SCN-044)

// The text the scene.name column shows: the focused scene's name (the SCENE MANAGER sets it; a ParamSpec value
// cannot hold text). Fixed storage, no allocation; message thread.
std::string_view focusedSceneName() noexcept;
void setFocusedSceneName(std::string_view) noexcept;
} // namespace core::commands::scene
