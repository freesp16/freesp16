// The shell's interaction model (04-interaction-model.md UID-150…181, popups-and-feedback.md UID-900…916,
// 03-ui/README.md UI-002…017, navigation UI-400…408).
#include "tests/unit/ui/ShellHarness.h"
#include "ui/shell/Layout.h"
#include "ui/widgets/Dialog.h"
#include "ui/widgets/ListPopup.h"
#include "ui/widgets/Message.h"
#include "ui/widgets/Progress.h"
#include "ui/widgets/SettingPopup.h"
#include "ui/widgets/Toast.h"

#include <catch2/catch_test_macros.hpp>

using uitest::Harness;
namespace L = ui::layout;

TEST_CASE("shell: HOME through the shell, encoder n edits column n of the selected track (UID-120, UID-160)",
          "[ui][shell]")
{
    Harness h;
    CHECK(h.shell->router().current().id == "home");
    CHECK(h.shell->stripTab() == "TRACK {track}");
    h.frame();
    CHECK(h.drewText("TRACK 1"));
    CHECK(h.drewText("HOME"));
    REQUIRE(h.shell->boundColumn(0) != nullptr);
    CHECK(h.shell->boundColumn(0)->id == "track.volume");
    CHECK(h.shell->boundColumn(5)->id == "track.triggerMode");
    h.turn(io::knob(1), -20);
    CHECK(h.value("track.volume") == 80.0f);
    h.frame(100);
    CHECK(h.shell->column(0).touched); // accent underline for 600 ms after the last change
    h.frame(600);
    CHECK_FALSE(h.shell->column(0).touched);
    // Selecting another track rebinds the strip to it (UI-205): pads select in the default pad mode.
    h.press(io::pad(5));
    h.frame();
    CHECK(h.shell->state().selectedTrack == 5);
    CHECK(h.drewText("TRACK 5"));
    h.turn(io::knob(2), 10);
    CHECK(h.value("track.pan", 5) == 10.0f);
    CHECK(h.value("track.pan", 1) == 0.0f);
    // Enumerations step one entry per detent (UID-162).
    h.turn(io::knob(6), 1);
    CHECK(h.value("track.triggerMode", 5) == 1.0f);
}

TEST_CASE("shell: SHIFT + turn is a fine step and every column shows FINE (UID-161)", "[ui][shell]")
{
    Harness h;
    const auto* pitch = core::builtinParams().find("playback.pitch");
    REQUIRE(pitch != nullptr);
    h.turn(io::knob(3), 1);
    const float coarse = h.value("playback.pitch");
    h.press(io::Control::Shift);
    h.frame();
    CHECK(h.shell->column(2).fine);
    CHECK(h.drewText("FINE"));
    h.turn(io::knob(3), 1);
    CHECK(h.value("playback.pitch") - coarse == pitch->step(coarse, 1, true) - coarse);
    h.release(io::Control::Shift);
    h.frame();
    CHECK_FALSE(h.shell->column(2).fine);
}

TEST_CASE("shell: a held step turns the tab into STEP n and encoders write locks (UID-166, UI-204)", "[ui][shell]")
{
    Harness h;
    h.press(io::step(3));
    h.frame();
    CHECK(h.shell->strip().model().stepHeld == 3);
    CHECK(h.drewText("STEP 3"));
    h.turn(io::knob(3), 50);
    h.frame();
    CHECK(h.shell->column(2).locked);
    CHECK(h.value("playback.pitch") == 0.0f); // the track value is untouched
    float lock = 0;
    REQUIRE(h.shell->values().lockOf(*core::builtinParams().find("playback.pitch"), h.shell->state().trackScope(), 3,
                                     lock));
    CHECK(lock != 0.0f);
    h.release(io::step(3));
    h.frame();
    CHECK(h.shell->strip().model().stepHeld == 0);
    CHECK_FALSE(h.shell->column(2).locked);
}

TEST_CASE("shell: PITCH is greyed and ignores its encoder while TIME STRETCH = RESAMPLE (UI-003)", "[ui][shell]")
{
    Harness h;
    h.turn(io::knob(4), 1); // TIME STRETCH OFF → RESAMPLE
    h.frame();
    REQUIRE(h.value("playback.timeStretch") == 1.0f);
    CHECK(h.shell->column(2).disabled);
    h.turn(io::knob(3), 5);
    CHECK(h.value("playback.pitch") == 0.0f);
}

TEST_CASE("shell: the rotary moves the selection on HOME and push opens TRACK MENU (UI-401, UID-150)", "[ui][shell]")
{
    Harness h;
    h.turn(io::Control::Selector, -1);
    CHECK(h.shell->state().selectedTrack == 16); // wraps 1 → 16
    h.turn(io::Control::Selector, 2);
    CHECK(h.shell->state().selectedTrack == 2);
    h.press(io::Control::SelectorPush);
    h.frame();
    CHECK(h.shell->router().current().id == "track.menu");
    CHECK(h.drewText("TRACK MENU"));
    h.press(io::Control::Back);
    h.frame();
    CHECK(h.shell->router().current().id == "home");
}

TEST_CASE("shell: two-tap — the first tap on a tile selects, the second opens (UID-155, UI-014)", "[ui][shell]")
{
    Harness h;
    const auto t5 = L::trackTile(5);
    h.touch(t5.x + 20, t5.y + 20);
    h.frame();
    CHECK(h.shell->state().selectedTrack == 5);
    CHECK(h.shell->router().current().id == "home");
    h.touch(t5.x + 20, t5.y + 20);
    h.frame();
    CHECK(h.shell->router().current().id == "track.menu");
    // slot A shows BACK on track screens (UI-343) and acts as BACK
    CHECK(h.shell->contextBar().model().back);
    h.touch(L::slotA.x + 10, L::slotA.y + 10);
    h.frame();
    CHECK(h.shell->router().current().id == "home");
}

TEST_CASE("shell: menu buttons open their screens and light while shown (UID-305, UI-207)", "[ui][shell]")
{
    Harness h;
    const auto mixer = L::menuButton(1);
    h.touch(mixer.x + 20, mixer.y + 20);
    h.frame();
    CHECK(h.shell->router().current().id == "mixer");
    CHECK(h.shell->contextBar().model().menuSelected == 1);
    CHECK(h.drewText("MIXER"));
    h.press(io::Control::Home);
    h.frame();
    CHECK(h.shell->router().current().id == "home");
    CHECK(h.shell->contextBar().model().menuSelected == -1);
    // SHIFT + HOME = UTILITY (UI-120)
    h.press(io::Control::Shift);
    h.press(io::Control::Home);
    h.release(io::Control::Shift);
    h.frame();
    CHECK(h.shell->router().current().id == "utility");
}

namespace
{
using K = ui::RailItemSpec::Kind;
constexpr ui::RailItemSpec kRail[] = {{K::Button, "TO MIXER", "mixer"},
                                      {K::Toggle, "LOOP", {}, "playback.loop"},
                                      {K::Option, "STRETCH", {}, "playback.timeStretch"}};
const ui::ScreenSpec kRailScreen{.id = "rail.test",
                                 .title = "RAIL",
                                 .parent = "home",
                                 .body = ui::BodyTemplate::RailContent,
                                 .contextBar = ui::ContextBarKind::TrackContext,
                                 .rail = kRail,
                                 .bodyComponent = {},
                                 .strip = {"RAIL", {"", "", "", "", "playback.loop"}}};
} // namespace

TEST_CASE("shell: rail focus, toggles, option selectors and focus memory (UID-004, UI-009)", "[ui][shell]")
{
    Harness h({kRailScreen});
    REQUIRE(h.shell->open("rail.test"));
    h.frame();
    CHECK(h.shell->railFocus() == 0);          // no body: the first rail item holds the focus
    CHECK(h.shell->boundColumn(0) == nullptr); // empty columns stay empty (UID-003)
    CHECK(h.shell->boundColumn(4) != nullptr);
    h.turn(io::Control::Selector, 1);
    CHECK(h.shell->railFocus() == 1);
    h.press(io::Control::SelectorPush); // toggle LOOP
    CHECK(h.value("playback.loop") == 1.0f);
    h.touch(L::railItem(1).x + 10, L::railItem(1).y + 10); // toggles act on the first tap
    CHECK(h.value("playback.loop") == 0.0f);
    h.turn(io::Control::Selector, 1);
    h.press(io::Control::SelectorPush); // the option opens its ListPopup
    h.frame();
    REQUIRE(h.shell->popupDepth() == 1);
    CHECK(h.hasScrim());
    h.turn(io::Control::Selector, 2);
    h.press(io::Control::SelectorPush); // choose MASTER TEMPO and close
    h.frame();
    CHECK(h.shell->popupDepth() == 0);
    CHECK(h.value("playback.timeStretch") == 2.0f);
    // BACK returns HOME; reopening restores the rail focus (UI-009).
    h.press(io::Control::Back);
    h.frame();
    h.shell->open("rail.test");
    h.frame();
    CHECK(h.shell->railFocus() == 2);
}

TEST_CASE("shell: a ListPopup opens on the current value, chooses on push, BACK closes unchanged (UID-902)",
          "[ui][shell][popup]")
{
    Harness h;
    int chosen = -1;
    bool cancelled = false;
    ui::ListRequest r;
    r.title = "TRACK TYPE";
    r.items = {{"SAMPLE", {}, {}, 0}, {"THRU", {}, {}, 1}, {"MIDI", {}, {}, 2}, {"AS-1", {}, {}, 3}};
    r.current = 1;
    r.onChoose = [&](int i, const ui::ListItem&) { chosen = i; };
    r.onCancel = [&] { cancelled = true; };
    h.shell->showList(r);
    h.frame();
    auto* list = dynamic_cast<ui::ListPopup*>(h.shell->topPopup());
    REQUIRE(list != nullptr);
    CHECK(list->focusedItem() == 1);
    CHECK(h.hasScrim());
    CHECK(h.drewText("TRACK TYPE"));
    // The strip is dimmed (no bindings) but the encoders keep editing the screen behind (UI-289, UID-125).
    h.turn(io::knob(1), -1);
    CHECK(h.value("track.volume") == 99.0f);
    // Pads keep working (UID-913).
    h.press(io::pad(7));
    CHECK(h.shell->state().selectedTrack == 7);
    h.turn(io::Control::Selector, 5); // no wrap
    CHECK(list->focusedItem() == 3);
    h.press(io::Control::Back);
    CHECK(cancelled);
    CHECK(chosen == -1);
    CHECK(h.shell->popupDepth() == 0);
    // A click outside closes too; a click on the focused row chooses.
    h.shell->showList(r);
    h.frame();
    const auto b = h.shell->topPopup()->bounds();
    h.touch(b.x + 100, b.y + 48 + 44 * 2 + 10); // focus MIDI
    h.touch(b.x + 100, b.y + 48 + 44 * 2 + 10); // choose it
    CHECK(chosen == 2);
}

TEST_CASE("shell: group headers are skipped and SHIFT + turn jumps by group (UID-904)", "[ui][shell][popup]")
{
    Harness h;
    ui::ListRequest r;
    r.title = "DESTINATION";
    r.grouped = true;
    for (int i = 0; i < 12; ++i)
        r.items.push_back({i < 4 ? "P" : (i < 8 ? "Q" : "R"), i < 4 ? "PLAYBACK" : (i < 8 ? "AMP" : "FX1"), {}, i});
    h.shell->showList(r);
    auto* list = dynamic_cast<ui::ListPopup*>(h.shell->topPopup());
    REQUIRE(list != nullptr);
    CHECK(list->focusedItem() == 0);
    CHECK(list->rowCount() == 15);
    h.turn(io::Control::Selector, 4); // crosses the AMP header
    CHECK(list->focusedItem() == 4);
    h.press(io::Control::Shift);
    h.turn(io::Control::Selector, 1);
    CHECK(list->focusedItem() == 8);
    h.turn(io::Control::Selector, -1);
    CHECK(list->focusedItem() == 4);
    h.release(io::Control::Shift);
    h.frame();
    CHECK(list->firstVisibleRow() > 0); // scrolled with one row of context
}

TEST_CASE("shell: a SettingPopup edits with push, turn, push; BACK restores (UID-903, UI-004)", "[ui][shell][popup]")
{
    Harness h;
    int committedRow = -1, committedOption = -1;
    ui::SettingRequest r;
    r.title = "SETTING";
    r.rows = {{"Mode", {"TRIGGER", "FREE"}, 0}, {"Trigger On", {"FULL", "HALF", "PARAM"}, 0}};
    r.onCommit = [&](int row, int option, float) { committedRow = row, committedOption = option; };
    h.shell->showSetting(r);
    auto* s = dynamic_cast<ui::SettingPopup*>(h.shell->topPopup());
    REQUIRE(s != nullptr);
    h.turn(io::Control::Selector, 1);
    CHECK(s->focusedRow() == 1);
    h.press(io::Control::SelectorPush);
    CHECK(s->editing());
    h.turn(io::Control::Selector, 2);
    h.press(io::Control::Back); // value side → name side, value restored
    CHECK_FALSE(s->editing());
    CHECK(s->row(1).current == 0);
    CHECK(committedRow == -1);
    h.press(io::Control::SelectorPush);
    h.turn(io::Control::Selector, 1);
    h.press(io::Control::SelectorPush);
    CHECK(committedRow == 1);
    CHECK(committedOption == 1);
    h.press(io::Control::Back);
    CHECK(h.shell->popupDepth() == 0);
}

TEST_CASE("shell: a Message defaults to the affirmative button, BACK is CANCEL (UID-906)", "[ui][shell][popup]")
{
    Harness h;
    int result = -1;
    ui::MessageRequest m;
    m.icon = ui::glyph::Kind::Caution;
    m.title = "Delete pattern 3 of scene 1?";
    m.buttons = {{"CANCEL"}, {"DELETE", ui::ButtonStyle::Destructive}};
    m.onResult = [&](int b) { result = b; };
    h.shell->showMessage(m);
    auto* msg = dynamic_cast<ui::Message*>(h.shell->topPopup());
    REQUIRE(msg != nullptr);
    CHECK(msg->focusedButton() == 1);
    h.press(io::Control::Back);
    CHECK(result == 0);
    h.shell->showMessage(m);
    h.press(io::Control::SelectorPush);
    CHECK(result == 1);
    // HOME cancels a popup and goes HOME (UID-913)
    h.shell->open("mixer");
    h.frame();
    h.shell->showMessage(m);
    result = -1;
    h.press(io::Control::Home);
    h.frame();
    CHECK(result == 0);
    CHECK(h.shell->router().current().id == "home");
}

TEST_CASE("shell: the name Dialog types from the keyboard, validates and covers the strip (UID-905, UI-122)",
          "[ui][shell][popup]")
{
    Harness h;
    std::string saved;
    int button = -1;
    ui::DialogRequest d;
    d.title = "SAVE AS";
    d.prompt = "Save current PROJECT as:";
    d.text = "Project_005";
    d.buttons = {{"CANCEL"}, {"SAVE", ui::ButtonStyle::Primary}, {"SAVE TO USB"}};
    d.validate = [](std::string_view t)
    { return t.find('/') != std::string_view::npos ? std::string("Characters / \\ : not allowed") : std::string(); };
    d.onResult = [&](int b, std::string_view t) { button = b, saved = t; };
    h.shell->showDialog(d);
    h.frame();
    auto* dialog = dynamic_cast<ui::Dialog*>(h.shell->topPopup());
    REQUIRE(dialog != nullptr);
    CHECK(dialog->field().allSelected());
    CHECK(dialog->coversStrip());
    CHECK_FALSE(h.drewText("TRACK 1")); // the keyboard sits over the knob strip
    h.shell->typeText("Beat");          // typing replaces the selection
    CHECK(dialog->field().text() == "Beat");
    // on-screen keys: "s" then ⌫ then "1" from the 123 layer
    auto keyCentre = [&](int i)
    {
        const auto r = dialog->keyboard().key(i).rect;
        return std::pair{r.x + r.w / 2, r.y + r.h / 2};
    };
    auto [sx, sy] = keyCentre(12); // row 1: a s d …
    h.touch(sx, sy);
    CHECK(dialog->field().text() == "Beats");
    auto [bx, by] = keyCentre(10); // ⌫
    h.touch(bx, by);
    auto [lx, ly] = keyCentre(32); // 123
    h.touch(lx, ly);
    auto [dx, dy] = keyCentre(0); // "1" on the digit layer
    h.touch(dx, dy);
    CHECK(dialog->field().text() == "Beat1");
    h.shell->typeText("/");
    CHECK_FALSE(dialog->valid());
    h.shell->textKey(ui::TextKey::Return); // primary disabled while invalid
    CHECK(button == -1);
    h.shell->textKey(ui::TextKey::Backspace);
    h.shell->textKey(ui::TextKey::Return);
    CHECK(button == 1);
    CHECK(saved == "Beat1");
    h.shell->showDialog(d);
    h.shell->textKey(ui::TextKey::Escape);
    CHECK(button == 0);
}

TEST_CASE("shell: toasts never block input and hide after 2 s; progress shows after 300 ms (UID-907, UID-908)",
          "[ui][shell][popup]")
{
    Harness h;
    ui::ToastRequest t;
    t.text = "Saved";
    auto* toast = h.shell->showToast(t);
    h.frame(10);
    CHECK(h.drewText("Saved"));
    CHECK_FALSE(h.hasScrim());
    h.turn(io::Control::Selector, 1); // input still reaches the screen
    CHECK(h.shell->state().selectedTrack == 2);
    (void)toast;
    h.frame(2200);
    h.frame(10);
    CHECK_FALSE(h.drewText("Saved"));

    bool cancelled = false;
    ui::ProgressRequest p;
    p.title = "LOADING";
    p.cancellable = true;
    p.onCancel = [&] { cancelled = true; };
    auto* progress = h.shell->showProgress(p);
    h.frame(100);
    CHECK_FALSE(h.drewText("LOADING"));
    h.frame(300);
    CHECK(h.drewText("LOADING"));
    h.press(io::Control::Home); // ignored until done
    CHECK(h.shell->popupDepth() == 1);
    progress->set(1.0f);
    progress->done();
    h.frame();
    CHECK(h.shell->popupDepth() == 0);
    CHECK_FALSE(cancelled);
}

TEST_CASE("shell: popup bindings rebind the strip, nesting stops at two (UID-125, UID-914)", "[ui][shell][popup]")
{
    static constexpr ui::PopupBinding bindings[] = {{0, "playback.pitch"}, {2, "track.pan"}};
    Harness h;
    ui::PopupSpec modal{ui::PopupSpec::Kind::Modal, "bpm.test", "BPM/QUANTIZE"};
    modal.bindings = bindings;
    REQUIRE(h.shell->present(modal));
    h.frame();
    CHECK(h.shell->boundColumn(0)->id == "playback.pitch");
    CHECK(h.shell->boundColumn(1) == nullptr);
    h.turn(io::knob(3), 4);
    CHECK(h.value("track.pan") == 4.0f);
    ui::ListRequest r;
    r.title = "NESTED";
    r.items = {{"A"}};
    h.shell->showList(r);
    h.shell->showList(r); // a third modal popup is refused
    CHECK(h.shell->popupDepth() == 2);
    h.press(io::Control::Back);
    h.press(io::Control::Back);
    h.frame();
    CHECK(h.shell->popupDepth() == 0);
    CHECK(h.shell->boundColumn(0)->id == "track.volume"); // back to the screen's own strip
}

TEST_CASE("shell: caution chips persist and SHIFT + BACK opens the CAUTIONS list (UID-909)", "[ui][shell][popup]")
{
    Harness h;
    h.shell->setCaution("disk", "DISK FULL");
    h.shell->setCaution("midi", "MIDI CLOCK LOST");
    h.shell->setCaution("cpu", "CPU OVERLOAD");
    h.frame();
    CHECK(h.drewText("+2"));
    CHECK(h.drewText("CPU OVERLOAD"));
    CHECK_FALSE(h.drewText("DISK FULL"));
    h.press(io::Control::Shift);
    h.press(io::Control::Back);
    h.release(io::Control::Shift);
    h.frame();
    REQUIRE(h.shell->popupDepth() == 1);
    CHECK(h.drewText("CAUTIONS"));
    h.press(io::Control::Back);
    h.shell->clearCaution("cpu");
    h.shell->clearCaution("midi");
    h.frame();
    CHECK(h.drewText("DISK FULL"));
    CHECK_FALSE(h.drewText("+2"));
}

TEST_CASE("shell: navigation and input are logged for traces", "[ui][shell]")
{
    Harness h;
    h.press(io::Control::SelectorPush);
    h.frame();
    h.press(io::Control::Back);
    h.frame();
    bool nav = false, back = false;
    for (const auto& l : h.log)
    {
        nav |= l.find("NAV track.menu (HOME > TRACK MENU)") != std::string::npos;
        back |= l == "INPUT back";
    }
    CHECK(nav);
    CHECK(back);
}

TEST_CASE("shell: the light palette is generated from the same tokens (UID-051)", "[ui][shell]")
{
    Harness h;
    h.shell->state().skin = ui::theme::Skin::Light;
    const auto& dl = h.frame();
    REQUIRE(! dl.commands().empty());
    CHECK(dl.commands()[0].argb == ui::theme::palette(false).bg);
    CHECK(ui::theme::palette(false).bg != ui::theme::palette(true).bg);
}

TEST_CASE("shell: screen push slides and fades the body in, input jumps it to the end (UID-065)", "[ui][shell]")
{
    Harness h;
    h.frame();
    h.shell->open("track.menu");
    h.frame(0);  // enters the screen at `now`
    h.frame(60); // mid-transition: the placeholder panel (x 16) is shifted right and faded
    bool shifted = false;
    for (const auto& c : h.dl.commands())
        if (c.op == ui::DrawCmd::Op::Fill && c.rect.y == L::contentTop && c.rect.w == 1248 && c.rect.x > 16)
            shifted = (c.argb >> 24) < 255;
    CHECK(shifted);
    h.turn(io::Control::Selector, 1); // any input ends the transition
    h.frame(1);
    bool settled = false;
    for (const auto& c : h.dl.commands())
        if (c.op == ui::DrawCmd::Op::Fill && c.rect.y == L::contentTop && c.rect.w == 1248)
            settled = c.rect.x == 16 && (c.argb >> 24) == 255;
    CHECK(settled);
}
