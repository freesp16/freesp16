// SCENE MANAGER against scene-manager-pattern.svg, scene-manager-seq.svg and scene-manager-track.svg, and its scripted
// behaviour: two-level selection, a commit while playing queues and blinks until the boundary (stopped: at once),
// PASTE is disabled for an empty clipboard or the other kind, copy / paste / delete through undoable commands with
// toasts, deleting a scene asks, the SEQ. selection copies and pastes steps, the TRACK tab needs a chosen track.
#include "tests/snapshot/m2/M2Fixture.h"
#include "ui/screens/scenemanager/SceneManagerAccess.h"
#include "ui/shell/Layout.h"

#include <catch2/catch_test_macros.hpp>

using namespace snapshot;
using m2test::M2Fixture;
namespace L = ui::layout;

namespace
{
constexpr float keyX(int n)
{
    return 236 + 64 * static_cast<float>(n - 1) + 28;
}
constexpr float kSceneRowY = 228, kPatternRowY = 364;

ui::SceneManagerView view(M2Fixture& f)
{
    ui::SceneManagerView v;
    REQUIRE(ui::sceneManagerView(f.shell->body(), v));
    return v;
}

void feed(M2Fixture& f, bool playing, int scene0, int pattern0, int pendingPattern0 = -1)
{
    engine::MeterFeed m{};
    engine::PlayheadFeed p{};
    p.blockIndex = 1;
    p.playing = playing;
    p.scene = static_cast<std::uint8_t>(scene0);
    p.pattern = static_cast<std::uint8_t>(pattern0);
    p.pendingScene = static_cast<std::int8_t>(pendingPattern0 >= 0 ? scene0 : -1);
    p.pendingPattern = static_cast<std::int8_t>(pendingPattern0);
    f.shell->feeds().inject(m, p);
}

// scene_manager.py: scenes 1–4 and patterns 1–4 of scene 1 have content, scene 1 is "Intro", pattern 3 has 2 bars.
void mockupScenes(M2Fixture& f)
{
    auto& s = f.session();
    s.execute(ui::m2::cmd::setSceneName(0, "Intro"));
    for (int p = 0; p < 4; ++p)
        s.execute(ui::m2::cmd::toggleStep(s.project(), 0, p, 0, 0, core::model::TriggerType::Full, 127));
    for (int sc = 1; sc < 4; ++sc)
        s.execute(ui::m2::cmd::toggleStep(s.project(), sc, 0, 0, 0, core::model::TriggerType::Full, 127));
    s.execute(ui::m2::cmd::setPatternLength(s.project(), 0, 2, 32));
}

// The SEQ. tab's demo sequence of scene 1 / pattern 1 (2 bars).
void mockupSequence(M2Fixture& f)
{
    auto& s = f.session();
    s.execute(ui::m2::cmd::setPatternLength(s.project(), 0, 0, 32));
    auto put = [&](int t, std::initializer_list<int> steps)
    {
        for (int st : steps)
            s.execute(ui::m2::cmd::toggleStep(s.project(), 0, 0, t - 1, st, core::model::TriggerType::Full, 127));
    };
    put(1, {0, 4, 8, 12, 16, 20, 24, 28});
    put(2, {4, 12, 20, 28});
    put(3, {0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30});
    put(5, {0, 16});
    put(6, {0, 16});
    put(10, {0, 3, 6, 10, 14, 19, 22, 26});
}

struct QueueGuard
{
    explicit QueueGuard(int v) { ui::setSceneQueueHandledByEngine(v); }
    ~QueueGuard() { ui::setSceneQueueHandledByEngine(-1); }
};
} // namespace

TEST_CASE("ui.m2.sceneManager: PATTERN tab matches scene-manager-pattern.svg", "[snapshot][m2]")
{
    Session session;
    QueueGuard queue(1);
    M2Fixture f;
    mockupScenes(f);
    auto& st = f.shell->state();
    st.playing = true;
    st.position.set("001:04:02");
    feed(f, true, 0, 0);
    REQUIRE(f.open("sceneManager"));
    REQUIRE(f.shell->router().current().bodyComponent == "SceneManager");
    f.tap(keyX(2), kSceneRowY);         // focus scene 2
    f.tap(320, 568);                    // COPY
    f.tap(keyX(1), kSceneRowY);         // focus scene 1
    f.press(io::Control::SelectorPush); // into the patterns
    f.turn(io::Control::Selector, 2);   // pattern 3
    f.press(io::Control::SelectorPush); // commit: queued while playing
    feed(f, true, 0, 0, 2);             // the engine reports the pending switch
    f.settle(6);                        // past the toast
    const auto v = view(f);
    CHECK(v.level == 1);
    CHECK(v.pendingPattern == 3);
    CHECK_FALSE(v.pasteEnabled); // clipboard holds a scene, focus in the pattern row (UI-286)
    const auto ours = f.image();
    const auto& mock = m2test::m2mockup("scene-manager-pattern");
    CHECK(compare(ours, mock, L::canvas, "m2-scene-pattern").match >= kGate);
    CHECK(compare(ours, mock, {224, 152, 1040, 360}, "m2-scene-pattern-keys").match >= kGate);
    CHECK(compare(ours, mock, {16, 520, 1248, 96}, "m2-scene-pattern-buttons").match >= kGate);
    CHECK(compare(ours, mock, L::knobStrip, "m2-scene-pattern-strip").match >= kGate);
}

TEST_CASE("ui.m2.sceneManager: SEQ. tab matches scene-manager-seq.svg", "[snapshot][m2]")
{
    Session session;
    M2Fixture f;
    mockupSequence(f);
    f.shell->state().selectedTrack = 5;
    REQUIRE(f.open("sceneManager"));
    f.tap(112, 270 + 51); // SEQ.
    f.settle();
    CHECK(view(f).tab == 1);
    const auto ours = f.image();
    const auto& mock = m2test::m2mockup("scene-manager-seq");
    CHECK(compare(ours, mock, L::canvas, "m2-scene-seq").match >= kGate);
    CHECK(compare(ours, mock, {224, 152, 1040, 360}, "m2-scene-seq-grid").match >= kGate);
    CHECK(compare(ours, mock, L::knobStrip, "m2-scene-seq-strip").match >= kGate);
    f.tap(112, 152 + 51); // back to PATTERN for the next test (the tab is remembered)
}

TEST_CASE("ui.m2.sceneManager: TRACK tab matches scene-manager-track.svg", "[snapshot][m2]")
{
    Session session;
    M2Fixture f;
    mockupScenes(f);
    REQUIRE(f.open("sceneManager"));
    f.tap(112, 388 + 51); // TRACK
    {
        const auto v = view(f);
        CHECK(v.tab == 2);
        CHECK_FALSE(v.copyEnabled); // nothing chosen (UID-607)
        CHECK_FALSE(v.deleteEnabled);
    }
    f.tap(744 + 60, 184 + 80 * 2 + 36); // track 5 (pad order: column 1, row 3 from the top)
    f.settle();
    const auto v = view(f);
    CHECK(v.chosenTrack == 5);
    CHECK(v.copyEnabled);
    CHECK_FALSE(v.pasteEnabled);
    const auto ours = f.image();
    const auto& mock = m2test::m2mockup("scene-manager-track");
    CHECK(compare(ours, mock, L::canvas, "m2-scene-track").match >= kGate);
    CHECK(compare(ours, mock, {224, 152, 1040, 360}, "m2-scene-track-map").match >= kGate);
    f.tap(112, 152 + 51);
}

TEST_CASE("ui.m2.sceneManager: a commit while playing queues and blinks until the boundary; stopped is immediate",
          "[m2]")
{
    QueueGuard queue(1);
    M2Fixture f;
    auto& st = f.shell->state();
    REQUIRE(f.open("sceneManager"));
    // Stopped: at once.
    f.tap(keyX(2), kPatternRowY); // focus pattern 2
    f.tap(keyX(2), kPatternRowY); // tap again: commit
    CHECK(st.pattern == 2);
    CHECK(f.get("scene.queue") == 1);
    // Playing: queued; the key blinks at 2 Hz until the engine switches.
    st.playing = true;
    feed(f, true, 0, 1);
    f.tap(keyX(4), kPatternRowY);
    f.tap(keyX(4), kPatternRowY);
    feed(f, true, 0, 1, 3);
    f.settle();
    CHECK(st.pattern == 2);
    CHECK(view(f).pendingPattern == 4);
    CHECK(ui::sceneManagerKey(f.shell->body(), true, 4, 10000).pending);
    CHECK_FALSE(ui::sceneManagerKey(f.shell->body(), true, 4, 10300).pending); // off phase of the blink
    CHECK(f.shell->body()->dirty(f.now));
    feed(f, true, 0, 3); // boundary reached
    f.settle();
    CHECK(st.pattern == 4);
    CHECK(view(f).pendingPattern == 0);
    // Closing before a commit changes nothing (UID-603).
    f.tap(keyX(7), kSceneRowY);
    f.press(io::Control::SelectorPush);
    f.press(io::Control::Back);
    f.settle();
    CHECK(st.scene == 1);
    CHECK(st.pattern == 4);
}

TEST_CASE("ui.m2.sceneManager: PASTE is disabled for the other kind; copy and paste a pattern, undo", "[m2]")
{
    M2Fixture f;
    auto& s = f.session();
    s.execute(ui::m2::cmd::toggleStep(s.project(), 0, 0, 2, 5, core::model::TriggerType::Half, 127));
    REQUIRE(f.open("sceneManager"));
    CHECK_FALSE(view(f).pasteEnabled); // empty clipboard
    f.tap(keyX(1), kPatternRowY);      // focus pattern 1 (pattern row)
    f.tap(320, 568);                   // COPY
    f.settle();
    CHECK(m2test::hasText(f.dl, "Pattern 1 copied"));
    CHECK(view(f).pasteEnabled);
    CHECK(ui::sceneManagerKey(f.shell->body(), true, 1, f.now).copied);
    f.tap(keyX(1), kSceneRowY); // the scene row: the clipboard holds the other kind
    CHECK_FALSE(view(f).pasteEnabled);
    f.tap(keyX(9), kPatternRowY);
    CHECK(view(f).pasteEnabled);
    f.tap(528, 568); // PASTE onto pattern 9
    f.settle();
    CHECK(s.cell(0, 8, 2, 5).kind == ui::StepKind::Half);
    CHECK(m2test::hasText(f.dl, "Pattern 9 pasted to scene 1"));
    REQUIRE(s.undo());
    CHECK(s.cell(0, 8, 2, 5).kind == ui::StepKind::Empty);
}

TEST_CASE("ui.m2.sceneManager: deleting a scene asks; deleting a pattern acts at once unless it is playing", "[m2]")
{
    M2Fixture f;
    auto& s = f.session();
    s.execute(ui::m2::cmd::toggleStep(s.project(), 2, 0, 0, 0, core::model::TriggerType::Full, 127));
    s.execute(ui::m2::cmd::toggleStep(s.project(), 0, 4, 0, 0, core::model::TriggerType::Full, 127));
    REQUIRE(f.open("sceneManager"));
    f.tap(keyX(3), kSceneRowY);
    f.tap(736, 568); // DELETE scene 3
    f.settle();
    REQUIRE(f.shell->popupDepth() == 1);
    CHECK(m2test::hasText(f.dl, "Delete scene 3 and its 16 patterns?"));
    f.turn(io::Control::Selector, 1);
    f.press(io::Control::SelectorPush);
    f.settle();
    CHECK_FALSE(s.sceneHasContent(2));
    f.tap(keyX(1), kSceneRowY);
    f.tap(keyX(5), kPatternRowY);
    f.tap(736, 568); // DELETE pattern 5: not playing, at once
    f.settle();
    CHECK(f.shell->popupDepth() == 0);
    CHECK_FALSE(s.patternHasContent(0, 4));
    REQUIRE(s.undo());
    CHECK(s.patternHasContent(0, 4));
    f.shell->state().playing = true; // the playing pattern asks (UI-287)
    f.tap(keyX(1), kPatternRowY);
    f.tap(736, 568);
    f.settle();
    CHECK(f.shell->popupDepth() == 1);
    CHECK(m2test::hasText(f.dl, "Delete pattern 1? It is playing."));
}

TEST_CASE("ui.m2.sceneManager: SEQ. selection copy and paste onto another track; TRACK tab copy and reset", "[m2]")
{
    M2Fixture f;
    auto& s = f.session();
    s.execute(ui::m2::cmd::toggleStep(s.project(), 0, 0, 0, 3, core::model::TriggerType::Full, 127));
    f.shell->state().selectedTrack = 1;
    REQUIRE(f.open("sceneManager"));
    f.tap(112, 270 + 51); // SEQ.
    f.settle();
    f.tap(320, 568); // COPY track 1, bar 1
    f.shell->state().selectedTrack = 4;
    f.settle();
    f.tap(528, 568); // PASTE onto track 4
    CHECK(s.cell(0, 0, 3, 3).kind == ui::StepKind::Full);
    f.tap(736, 568); // DELETE the selection of track 4
    CHECK(s.cell(0, 0, 3, 3).kind == ui::StepKind::Empty);
    CHECK(s.cell(0, 0, 0, 3).kind == ui::StepKind::Full);

    f.tap(112, 388 + 51); // TRACK
    f.press(io::pad(1));  // a pad chooses track 1
    f.settle();
    CHECK(view(f).chosenTrack == 1);
    f.tap(320, 568);                // COPY (with sequence)
    f.tap(224 + 64 + 28, 328 + 40); // scene 2
    f.press(io::pad(2));
    f.tap(528, 568); // PASTE track 1 of scene 1 onto track 2 of scene 2 (empty: no question)
    f.settle();
    CHECK(s.cell(1, 0, 1, 3).kind == ui::StepKind::Full);
    f.tap(736, 568); // DELETE asks: reset to defaults
    f.settle();
    REQUIRE(f.shell->popupDepth() == 1);
    CHECK(m2test::hasText(f.dl, "Reset track 2 of scene 2 to defaults?"));
    f.turn(io::Control::Selector, 1);
    f.press(io::Control::SelectorPush);
    CHECK(s.cell(1, 0, 1, 3).kind == ui::StepKind::Empty);
    f.tap(112, 152 + 51);
}

TEST_CASE("ui.m2.sceneManager: NAME opens the name dialog; COLOR edits the focused scene; SCENE BPM sets and clears",
          "[m2]")
{
    M2Fixture f;
    auto& s = f.session();
    REQUIRE(f.open("sceneManager"));
    f.tap(keyX(2), kSceneRowY);
    f.settle();
    CHECK(f.shell->stripTab() == "SCENE 02");
    f.turn(io::knob(6), 3); // COLOR of scene 2
    f.settle();
    CHECK(s.project().scenes[1].colour == 4);
    f.turn(io::knob(5), 1); // NAME: opens the dialog
    f.settle();
    REQUIRE(f.shell->popupDepth() == 1);
    CHECK(m2test::hasText(f.dl, "Name of scene 02:"));
    f.shell->typeText("Verse");
    f.press(io::Control::SelectorPush); // push = primary (OK)
    f.settle();
    CHECK(s.project().scenes[1].name == "Verse");
    CHECK(m2test::hasText(f.dl, "SCENE 02  Verse"));
    f.tap(960, 568); // SCENE BPM popup (a custom popup through ShellApi::showPopup)
    f.settle();
    REQUIRE(f.shell->popupDepth() == 1);
    CHECK(m2test::hasText(f.dl, "---"));
    f.turn(io::Control::Selector, 4); // from the project tempo 120.0
    f.press(io::Control::Shift);
    f.turn(io::Control::Selector, 5);
    f.release(io::Control::Shift);
    f.settle();
    CHECK(m2test::hasText(f.dl, "124.5"));
    f.press(io::Control::SelectorPush); // OK
    f.settle();
    CHECK(f.shell->popupDepth() == 0);
    REQUIRE(s.project().scenes[1].bpm.has_value());
    CHECK(*s.project().scenes[1].bpm == 124.5);
    CHECK(m2test::hasText(f.dl, "124.5")); // the SCENE BPM selector shows it
    f.tap(960, 568);
    f.tap(320 + 24 + 64, 224 + 320 - 48); // OFF
    f.tap(960 - 24 - 64, 224 + 320 - 48); // OK
    f.settle();
    CHECK_FALSE(s.project().scenes[1].bpm.has_value());
    // A drag on the value turns it (8 px per detent, upward = up).
    f.tap(960, 568);
    f.touch(640, 380, ui::touch::kDown);
    f.touch(640, 380 - 8 * 1.6f * 3 - 6, ui::touch::kDrag);
    f.touch(640, 380 - 8 * 1.6f * 3 - 6, ui::touch::kUp);
    f.press(io::Control::SelectorPush);
    REQUIRE(s.project().scenes[1].bpm.has_value());
    CHECK(*s.project().scenes[1].bpm == 123.0);
}

TEST_CASE(
    "ui.m2.sceneManager: the tabs are rail items; push on a focused tab switches, a drag moves the SEQ. selection",
    "[m2]")
{
    M2Fixture f;
    REQUIRE(f.open("sceneManager"));
    f.tap(112, 270 + 51); // SEQ. (a rail item: the shell gives it the focus)
    CHECK(view(f).tab == 1);
    CHECK(f.shell->railFocus() == 1);
    f.turn(io::Control::Selector, 1); // rail focus → TRACK
    f.press(io::Control::SelectorPush);
    CHECK(view(f).tab == 2);
    f.turn(io::Control::Selector, 1); // past the last tab: the body takes the rotary again
    CHECK(f.shell->railFocus() == -1);
    f.tap(112, 270 + 51);
    auto& s = f.session();
    s.execute(ui::m2::cmd::setPatternLength(s.project(), 0, 0, 64));
    f.settle();
    f.touch(300, 300, ui::touch::kDown);
    f.touch(300 + 240 * 2, 300, ui::touch::kDrag);
    f.touch(300 + 240 * 2, 300, ui::touch::kUp);
    CHECK(view(f).seqStartBar == 2);
    f.tap(320, 568); // COPY: bar 3 of the selection
    f.settle();
    CHECK(m2test::hasText(f.dl, "1 bar copied"));
    f.tap(112, 152 + 51);
}
