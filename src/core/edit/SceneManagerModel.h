// The SCENE MANAGER's logic (spec 02-features/scenes-and-patterns.md SCN-040…044, editing-and-undo.md
// EDT-010…012; 03-ui/screens.md UI-284…288): the two-level selection, the button states and the action handlers
// `sceneManager.copy | paste | delete | commit` the screen binds. No drawing; message thread only.
//
//   selection (SCN-043, UI-284)   turn the selector over the 16 scenes → push = into that scene's patterns (focus on
//                                 the current pattern when it is the current scene, else pattern 1) → turn → push =
//                                 commit (scene.queue: queued to PATTERN QUANTIZE while playing, SCN-020). Taps: a tap
//                                 focuses, a tap on the focused button enters / commits. close() before a commit
//                                 changes nothing.
//   buttons (SCN-044)             focus frame, current indicator (flashing while a switch to it is pending), lit in the
//                                 scene colour when it has content (a scene with ≥ 1 recorded pattern, a pattern with a
//                                 sequence), the copy mark (EDT-010).
//   COPY / PASTE / DELETE         act on the focus (a scene in the scene row, a pattern in the pattern row). PASTE is
//                                 disabled when the clipboard is empty or holds the other kind (EDT-011). DELETE resets
//                                 the slot (EDT-012) and asks first (UI-296) for a scene or the playing pattern.
#pragma once

#include "core/commands/Command.h"
#include "core/edit/SceneEditor.h"
#include "core/model/Project.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace core::edit
{
// What the screen knows each frame (indices 0-based; pending −1 = none: the engine's PlayheadFeed).
struct SceneManagerInputs
{
    const model::Project* project = nullptr;
    int currentScene = 0, currentPattern = 0;
    int pendingScene = -1, pendingPattern = -1;
    bool playing = false;
};

class SceneManagerModel
{
public:
    enum class Level : std::uint8_t { Scene, Pattern };

    void open(int currentScene, int currentPattern) noexcept; // focus the current scene in the scene row
    void turn(int detents) noexcept;                           // within the row, clamped to 1…16
    // Push: the scene row → its pattern row; the pattern row → commit. Returns the commit (scene, pattern).
    std::optional<std::pair<int, int>> push(int currentScene, int currentPattern) noexcept;
    // A tap on scene n (patternRow false) or pattern n of the focused scene: focus, or enter / commit when focused.
    std::optional<std::pair<int, int>> tap(bool patternRow, int n, int currentScene, int currentPattern) noexcept;
    void back() noexcept { level_ = Level::Scene; } // pattern row → scene row
    void close() noexcept {}                         // nothing was committed: nothing changes (SCN-043)

    Level level() const noexcept { return level_; }
    int focusScene() const noexcept { return scene_; }
    int focusPattern() const noexcept { return pattern_; }
    EditRef focusRef() const noexcept { return level_ == Level::Scene ? sceneRef(scene_) : patternRef(scene_, pattern_); }

    SceneManagerButton sceneButton(int scene, const SceneManagerInputs&, const SceneEditor&) const noexcept;
    SceneManagerButton patternButton(int pattern, const SceneManagerInputs&, const SceneEditor&) const noexcept;

    bool copyEnabled() const noexcept { return true; }
    bool pasteEnabled(const SceneEditor& e) const noexcept { return e.canPaste(focusRef()); } // EDT-011
    bool deleteEnabled() const noexcept { return true; }
    // EDT-012 / UI-287: a scene, or the pattern now playing.
    bool deleteNeedsConfirm(const SceneManagerInputs&) const noexcept;

private:
    Level level_ = Level::Scene;
    int scene_ = 0, pattern_ = 0;
};

// The action ids the screen binds (the UI framework dispatches actions by id).
inline constexpr std::string_view kActionCopy = "sceneManager.copy", kActionPaste = "sceneManager.paste",
                                  kActionDelete = "sceneManager.delete", kActionCommit = "sceneManager.commit";

struct SceneManagerHooks
{
    // UI-296 message popup: run() when the user confirms (title, one line, the destructive button's caption).
    std::function<void(std::string_view title, std::string line, std::string_view action, std::function<void()> run)>
        confirm;
    std::function<void(std::string text)> toast; // "Scene 3 copied", "Pattern 2 pasted to scene 1", …
};

// Runs action `id` on the model's focus; false for an unknown id or a disabled action. `commit` posts the queued switch
// (core::commands::scene::selectScenePattern) for the focused scene / pattern.
bool runSceneManagerAction(std::string_view id, SceneManagerModel&, const SceneManagerInputs&, SceneEditor&,
                           const SceneManagerHooks& = {});

// EDT-009: clearing a pattern or a track's sequence confirms first, or is only undoable — the preference
// `confirmPreference` (default true). A scene always confirms (EDT-012); other resets (steps, page, a track's
// settings) are undoable without asking. The SCENE MANAGER's playing-pattern rule is deleteNeedsConfirm.
constexpr bool clearNeedsConfirm(ObjectKind k, bool confirmPreference) noexcept
{
    switch (k)
    {
        case ObjectKind::Scene: return true;
        case ObjectKind::Pattern:
        case ObjectKind::Sequence:
        case ObjectKind::TrackWithSequences: return confirmPreference;
        default: return false;
    }
}

// SCN-044 "has content": a pattern with at least one trigger; a scene with at least one such pattern.
bool patternHasSequence(const model::Pattern&) noexcept;
bool sceneHasRecordedPattern(const model::Scene&) noexcept;
} // namespace core::edit
