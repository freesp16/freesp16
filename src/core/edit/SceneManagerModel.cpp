#include "core/edit/SceneManagerModel.h"

#include "core/commands/scene/SceneCommands.h"

#include <algorithm>
#include <cstdio>

namespace core::edit
{
namespace
{
std::string text(const char* fmt, int a, int b = 0)
{
    char buf[80];
    const int n = std::snprintf(buf, sizeof buf, fmt, a, b);
    return std::string(buf, static_cast<std::size_t>(n > 0 ? n : 0));
}
} // namespace

bool patternHasSequence(const model::Pattern& p) noexcept
{
    return core::commands::scene::patternHasContent(p);
}

bool sceneHasRecordedPattern(const model::Scene& s) noexcept
{
    return std::any_of(s.patterns.begin(), s.patterns.end(), [](const model::Pattern& p) { return patternHasSequence(p); });
}

void SceneManagerModel::open(int currentScene, int currentPattern) noexcept
{
    level_ = Level::Scene;
    scene_ = currentScene & 15;
    pattern_ = currentPattern & 15;
}

void SceneManagerModel::turn(int detents) noexcept
{
    int& f = level_ == Level::Scene ? scene_ : pattern_;
    f = std::clamp(f + detents, 0, 15);
}

std::optional<std::pair<int, int>> SceneManagerModel::push(int currentScene, int currentPattern) noexcept
{
    if (level_ == Level::Scene)
    {
        level_ = Level::Pattern;
        pattern_ = scene_ == (currentScene & 15) ? (currentPattern & 15) : 0;
        return std::nullopt;
    }
    return std::make_pair(scene_, pattern_);
}

std::optional<std::pair<int, int>> SceneManagerModel::tap(bool patternRow, int n, int currentScene,
                                                         int currentPattern) noexcept
{
    const int i = (n - 1) & 15;
    if (! patternRow)
    {
        if (level_ == Level::Scene && scene_ == i)
            return push(currentScene, currentPattern); // the focused scene again: into its patterns
        level_ = Level::Scene;
        scene_ = i;
        return std::nullopt;
    }
    if (level_ == Level::Pattern && pattern_ == i)
        return std::make_pair(scene_, pattern_); // the focused pattern again: commit
    level_ = Level::Pattern;
    pattern_ = i;
    return std::nullopt;
}

SceneManagerButton SceneManagerModel::sceneButton(int scene, const SceneManagerInputs& in,
                                                  const SceneEditor& editor) const noexcept
{
    const int s = scene & 15;
    SceneManagerButton b;
    b.focus = level_ == Level::Scene && scene_ == s;
    b.current = in.currentScene == s;
    const int target = in.pendingScene >= 0 ? in.pendingScene : in.currentScene;
    b.pending = in.pendingPattern >= 0 && target == s && target != in.currentScene; // a switch to this scene waits
    if (in.project != nullptr)
    {
        const auto& sc = in.project->scenes[static_cast<std::size_t>(s)];
        b.hasContent = sceneHasRecordedPattern(sc);
        b.colour = static_cast<std::uint32_t>(sc.colour & 15);
    }
    b.copied = editor.isCopied(sceneRef(s));
    return b;
}

SceneManagerButton SceneManagerModel::patternButton(int pattern, const SceneManagerInputs& in,
                                                    const SceneEditor& editor) const noexcept
{
    const int p = pattern & 15;
    SceneManagerButton b;
    b.focus = level_ == Level::Pattern && pattern_ == p;
    b.current = in.currentScene == scene_ && in.currentPattern == p;
    const int pendingScene = in.pendingScene >= 0 ? in.pendingScene : in.currentScene;
    b.pending = in.pendingPattern == p && pendingScene == scene_;
    if (in.project != nullptr)
    {
        const auto& sc = in.project->scenes[static_cast<std::size_t>(scene_)];
        b.hasContent = patternHasSequence(sc.patterns[static_cast<std::size_t>(p)]);
        b.colour = static_cast<std::uint32_t>(sc.colour & 15);
    }
    b.copied = editor.isCopied(patternRef(scene_, p));
    return b;
}

bool SceneManagerModel::deleteNeedsConfirm(const SceneManagerInputs& in) const noexcept
{
    if (level_ == Level::Scene)
        return true;
    return in.playing && in.currentScene == scene_ && in.currentPattern == pattern_;
}

bool runSceneManagerAction(std::string_view id, SceneManagerModel& m, const SceneManagerInputs& in, SceneEditor& editor,
                           const SceneManagerHooks& hooks)
{
    const bool scene = m.level() == SceneManagerModel::Level::Scene;
    const int s = m.focusScene(), p = m.focusPattern();
    auto toast = [&hooks](std::string t) {
        if (hooks.toast)
            hooks.toast(std::move(t));
    };
    if (id == kActionCopy)
    {
        editor.copy(m.focusRef());
        toast(scene ? text("Scene %d copied", s + 1) : text("Pattern %d copied", p + 1));
        return true;
    }
    if (id == kActionPaste)
    {
        if (! m.pasteEnabled(editor))
            return false;
        const auto* src = editor.clipboard().source();
        const int from = src != nullptr ? (scene ? src->scene : src->pattern) + 1 : 0;
        editor.paste(m.focusRef(), [hooks, scene, s, p, from](Result r) {
            if (! hooks.toast)
                return;
            if (! r.ok())
                hooks.toast(r.error().message);
            else
                hooks.toast(scene ? text("Scene %d pasted to scene %d", from, s + 1)
                                  : text("Pattern %d pasted to pattern %d", from, p + 1));
        });
        return true;
    }
    if (id == kActionDelete)
    {
        const EditRef ref = m.focusRef();
        auto run = [&editor, ref, hooks, scene, s, p] {
            editor.reset(ref); // EDT-012: the slot back to its empty default; undoable
            if (hooks.toast)
                hooks.toast(scene ? text("Scene %d deleted", s + 1) : text("Pattern %d deleted", p + 1));
        };
        if (m.deleteNeedsConfirm(in) && hooks.confirm)
            hooks.confirm(scene ? "DELETE SCENE" : "DELETE PATTERN",
                          scene ? text("Delete scene %d and its 16 patterns?", s + 1)
                                : text("Delete pattern %d? It is playing.", p + 1),
                          "DELETE", run);
        else if (! m.deleteNeedsConfirm(in))
            run();
        else
            return false; // a confirmation is required and nobody can ask
        return true;
    }
    if (id == kActionCommit)
    {
        if (scene)
            return false; // a commit needs a pattern (SCN-043)
        editor.bus().execute(core::commands::scene::selectScenePattern(s, p));
        return true;
    }
    return false;
}
} // namespace core::edit
