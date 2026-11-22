// SceneManagerModel (SCN-040…044, EDT-010…012, UI-284…288): two-level selection with commit at PATTERN QUANTIZE (a
// queued switch request), closing without commit changes nothing, the button states, and the action handlers
// sceneManager.copy | paste | delete | commit (PASTE disabled for the other kind; delete of a scene or the playing
// pattern asks first).
#include "tests/unit/edit/EditTestUtil.h"

#include "core/commands/scene/SceneCommands.h"
#include "core/edit/SceneManagerModel.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

namespace ce = core::edit;
namespace m = core::model;
namespace sc = core::commands::scene;

namespace
{
struct Rig
{
    edittest::Bus b;
    ce::InlineWorkerPool pool;
    ce::SceneEditor editor {b.bus, pool};
    ce::SceneManagerModel model;
    std::vector<std::string> toasts, asked;
    std::function<void()> pendingConfirm;
    ce::SceneManagerHooks hooks {
        [this](std::string_view title, std::string line, std::string_view, std::function<void()> run) {
            asked.push_back(std::string(title) + ": " + line);
            pendingConfirm = std::move(run);
        },
        [this](std::string t) { toasts.push_back(std::move(t)); }};
    ce::SceneManagerInputs in()
    {
        ce::SceneManagerInputs i;
        i.project = &b.project();
        i.currentScene = 0;
        i.currentPattern = 2;
        i.playing = playing;
        return i;
    }
    bool run(std::string_view id) { return ce::runSceneManagerAction(id, model, in(), editor, hooks); }
    bool playing = false;
};
} // namespace

TEST_CASE("edit.sceneManager: scene → push → pattern → push commits as a queued switch, close changes nothing",
          "[edit]")
{
    Rig r;
    std::vector<int> queued;
    auto sub = r.b.bus.observe("queue[", [&queued](const core::ChangeSet& c) {
        for (const auto& p : c.paths())
            if (auto v = sc::queueValueOf(p))
                queued.push_back(*v);
    });
    r.model.open(0, 2);
    CHECK(r.model.level() == ce::SceneManagerModel::Level::Scene);
    CHECK(r.model.focusScene() == 0);
    r.model.turn(3);
    CHECK(r.model.focusScene() == 3);
    r.model.turn(-10);
    CHECK(r.model.focusScene() == 0);
    r.model.turn(20);
    CHECK(r.model.focusScene() == 15);
    r.model.turn(-14); // scene 2
    CHECK_FALSE(r.model.push(0, 2).has_value());
    CHECK(r.model.level() == ce::SceneManagerModel::Level::Pattern);
    CHECK(r.model.focusPattern() == 0); // not the current scene: pattern 1
    r.model.turn(4);
    CHECK(r.run(ce::kActionCommit));
    core::commands::flushNotifications(r.b.bus);
    CHECK(queued == std::vector<int> {1 * 16 + 4});
    CHECK(r.b.undoDepth() == 0);     // a switch is not an edit
    CHECK_FALSE(r.b.bus.dirty());
    // Push commit returns the pair too; the current scene's pattern row focuses the current pattern.
    r.model.open(0, 2);
    r.model.push(0, 2);
    CHECK(r.model.focusPattern() == 2);
    const auto c = r.model.push(0, 2);
    REQUIRE(c.has_value());
    CHECK(*c == std::make_pair(0, 2));
    // Taps: focus, then enter; a tap on a pattern focuses, a second tap commits.
    r.model.open(0, 2);
    CHECK_FALSE(r.model.tap(false, 5, 0, 2).has_value());
    CHECK(r.model.focusScene() == 4);
    CHECK_FALSE(r.model.tap(false, 5, 0, 2).has_value());
    CHECK(r.model.level() == ce::SceneManagerModel::Level::Pattern);
    CHECK_FALSE(r.model.tap(true, 7, 0, 2).has_value());
    const auto t = r.model.tap(true, 7, 0, 2);
    REQUIRE(t.has_value());
    CHECK(*t == std::make_pair(4, 6));
    const m::Project before = r.b.project();
    r.model.close();
    CHECK(r.b.project() == before);
}

TEST_CASE("edit.sceneManager: button states: focus, current, pending, content in the scene colour, copied", "[edit]")
{
    Rig r;
    auto& p = r.b.project();
    for (auto& pat : p.scenes[7].patterns)
        pat = m::defaultPattern(); // scene 8: no recorded pattern
    p.scenes[0].patterns[9] = m::defaultPattern();
    r.model.open(0, 2);
    auto in = r.in();
    in.pendingScene = 3;
    in.pendingPattern = 5;
    auto s0 = r.model.sceneButton(0, in, r.editor);
    CHECK(s0.focus);
    CHECK(s0.current);
    CHECK_FALSE(s0.pending);
    CHECK(s0.hasContent);
    CHECK(s0.colour == static_cast<std::uint32_t>(p.scenes[0].colour));
    CHECK(r.model.sceneButton(3, in, r.editor).pending);
    CHECK_FALSE(r.model.sceneButton(7, in, r.editor).hasContent);
    r.model.push(0, 2);
    CHECK_FALSE(r.model.sceneButton(0, in, r.editor).focus);
    auto p2 = r.model.patternButton(2, in, r.editor);
    CHECK(p2.focus);
    CHECK(p2.current);
    CHECK_FALSE(r.model.patternButton(9, in, r.editor).hasContent);
    CHECK(r.model.patternButton(1, in, r.editor).hasContent);
    in.pendingScene = -1; // a pattern switch inside the current scene
    CHECK(r.model.patternButton(5, in, r.editor).pending);
    CHECK_FALSE(r.model.sceneButton(0, in, r.editor).pending);
    r.run(ce::kActionCopy);
    CHECK(r.model.patternButton(2, in, r.editor).copied);
    CHECK_FALSE(r.model.patternButton(3, in, r.editor).copied);
    CHECK_FALSE(r.model.sceneButton(0, in, r.editor).copied);
}

TEST_CASE("edit.sceneManager: COPY / PASTE / DELETE handlers, PASTE greyed for the other kind (EDT-010…012)",
          "[edit]")
{
    Rig r;
    r.model.open(0, 2);
    CHECK_FALSE(r.model.pasteEnabled(r.editor)); // empty clipboard
    CHECK_FALSE(r.run(ce::kActionPaste));
    CHECK(r.run(ce::kActionCopy)); // scene 1
    CHECK(r.toasts.back() == "Scene 1 copied");
    CHECK(r.model.pasteEnabled(r.editor));
    r.model.push(0, 2); // pattern row: a scene on the clipboard
    CHECK_FALSE(r.model.pasteEnabled(r.editor));
    CHECK_FALSE(r.run(ce::kActionPaste));
    CHECK(r.run(ce::kActionCopy)); // pattern 3
    r.model.turn(5);               // pattern 8
    REQUIRE(r.model.pasteEnabled(r.editor));
    const m::Pattern source = r.b.pattern(0, 2);
    CHECK(r.run(ce::kActionPaste));
    CHECK(r.b.pattern(0, 7) == source);
    CHECK(r.toasts.back() == "Pattern 3 pasted to pattern 8");
    // DELETE of a pattern that is not playing acts at once (undoable).
    CHECK(r.run(ce::kActionDelete));
    CHECK(r.asked.empty());
    CHECK(r.b.pattern(0, 7) == m::defaultPattern());
    REQUIRE(r.b.bus.undo());
    CHECK(r.b.pattern(0, 7) == source);
    // DELETE of the playing pattern asks first (UI-287).
    r.playing = true;
    r.model.turn(-5); // pattern 3 = the current one
    CHECK(r.model.deleteNeedsConfirm(r.in()));
    const m::Pattern playing = r.b.pattern(0, 2);
    CHECK(r.run(ce::kActionDelete));
    REQUIRE(r.asked.size() == 1);
    CHECK(r.asked.back() == "DELETE PATTERN: Delete pattern 3? It is playing.");
    CHECK(r.b.pattern(0, 2) == playing); // nothing until confirmed
    r.pendingConfirm();
    CHECK(r.b.pattern(0, 2) == m::defaultPattern());
    // DELETE of a scene always asks.
    r.model.back();
    CHECK(r.model.deleteNeedsConfirm(r.in()));
    CHECK(r.run(ce::kActionDelete));
    CHECK(r.asked.back() == "DELETE SCENE: Delete scene 1 and its 16 patterns?");
    // Without a way to ask, a delete that needs confirmation does nothing.
    const m::Project before = r.b.project();
    CHECK_FALSE(ce::runSceneManagerAction(ce::kActionDelete, r.model, r.in(), r.editor, {}));
    CHECK(r.b.project() == before);
    CHECK_FALSE(r.run("sceneManager.unknown"));
}
