// ARRANGER (arranger.md UID-575…583, screens.md UI-300…306): the mockup's song (5 rows, row 2 playing on its
// first of 2 passes, row 4 focused) against arranger.svg; the table's computed POSITION / LENGTH; knobs 1–4 bound to the
// focused row and END OF ARRANGEMENT appending; COPY / PASTE / DELETE + undo; SHIFT + DELETE confirmation; the
// follow-the-song rule; the keyboard-only workflow W10 (UI-420).
#include "core/commands/arrangement/ArrangementCommands.h"
#include "core/commands/scene/SceneCommands.h"
#include "tests/snapshot/m5/M5Fixture.h"
#include "ui/shell/Layout.h"

#include <catch2/catch_test_macros.hpp>

using namespace snapshot;
using m5test::M5Fixture;
namespace L = ui::layout;
namespace ar = core::commands::arrangement;

namespace
{
core::model::Section row(int scene, int pattern, int repeats, double bpm = 0)
{
    core::model::Section s;
    s.scene = static_cast<std::uint8_t>(scene - 1);
    s.pattern = static_cast<std::uint8_t>(pattern - 1);
    s.repeats = repeats;
    if (bpm > 0)
        s.fixedBpm = bpm;
    return s;
}
void mockupSong(M5Fixture& m)
{
    auto& s = m.session();
    s.execute(core::commands::scene::setPatternLength(0, 1, 32)); // scene 1 pattern 2: 2 bars
    s.execute(core::commands::scene::setPatternLength(1, 0, 32)); // scene 2 pattern 1: 2 bars
    s.execute(ar::appendRow(s.project(), row(1, 1, 2, 120.0)));
    s.execute(ar::appendRow(s.project(), row(1, 2, 2)));
    s.execute(ar::appendRow(s.project(), row(2, 1, 1)));
    s.execute(ar::appendRow(s.project(), row(2, 3, 4, 124.0)));
    s.execute(ar::appendRow(s.project(), row(1, 4, 1)));
    REQUIRE(ar::rowCount(s.project()) == 5);
}
int rows(M5Fixture& m)
{
    return ar::rowCount(m.project());
}
} // namespace

TEST_CASE("snapshot.m5.arranger: ARRANGER (song playing, row 4 focused) matches arranger.svg", "[snapshot][m5]")
{
    Session session;
    M5Fixture m;
    mockupSong(m);
    m.set("mixer.headphoneVolume", 1, 127);
    REQUIRE(m.open("arranger"));
    m.turnSel(3); // row 004
    auto& song = core::arrangement::liveSongStatus();
    song.row = 1;
    song.remaining = 1;
    m.play(17.5);
    m.state().position.set("005:02:03");
    const auto ours = m.f.image();
    song.row = -1;
    CHECK(ui::arrangerFocus(m.shell().body()) == 3);
    CHECK(m.shell().stripTab() == "ROW 004");
    CHECK(m.hasText("009.01")); // POSITION of row 4 = 2 + 4 + 2 bars in
    CHECK(m.hasText("004.00")); // its LENGTH = 4 × 1 bar
    CHECK(m.hasText("1 / 2"));  // row 2: the remaining passes (UID-579)
    CHECK(m.hasText("END OF ARRANGEMENT"));
    const auto& mock = m5test::m5mockup("arranger");
    CHECK(compare(ours, mock, {224, 152, 936, 456}, "m5-arranger-table").match >= kGate);
    CHECK(compare(ours, mock, {16, 152, 192, 456}, "m5-arranger-rail").match >= kGate);
    CHECK(compare(ours, mock, L::knobStrip, "m5-arranger-strip").match >= kGate);
    CHECK(compare(ours, mock, L::canvas, "m5-arranger").match >= kGate);
}

TEST_CASE("m5ui.arranger: knobs edit the focused row, END appends, COPY / PASTE / DELETE are single undo steps",
          "[m5]")
{
    M5Fixture m;
    REQUIRE(m.open("arranger"));
    CHECK(m.shell().current().parent == "home");
    CHECK(m.shell().boundColumn(0)->id == "arrangement.row.scene");
    CHECK(m.shell().boundColumn(4)->id == "mixer.headphoneVolume");
    CHECK(m.shell().boundColumn(5)->id == "mixer.masterVolume");
    // Empty: only END, focused; columns 1–4 show "–" (UID-577).
    CHECK(ui::arrangerFocus(m.shell().body()) == 0);
    CHECK(m.shell().stripTab() == "END");
    CHECK(m.shell().column(0).valueText == "\xE2\x80\x93");
    // UI-302: turning REPEAT on END appends a row with the current scene/pattern and that value.
    m.knob(3, 2);
    m.f.settle();
    REQUIRE(rows(m) == 1);
    CHECK(m.project().arrangement.sections[0].repeats == 3);
    CHECK_FALSE(m.project().arrangement.sections[0].fixedBpm.has_value());
    // The focus stays on END (now row 2): push inserts the current scene/pattern before it (UI-303).
    CHECK(ui::arrangerFocus(m.shell().body()) == 1);
    m.push();
    m.f.settle();
    REQUIRE(rows(m) == 2);
    CHECK(ui::arrangerFocus(m.shell().body()) == 2); // still END
    // Up to row 1: the knobs show and edit it (UI-301).
    m.turnSel(-2);
    m.f.settle();
    CHECK(m.shell().stripTab() == "ROW 001");
    CHECK(m.shell().column(2).valueText == "3");
    m.knob(1, 1); // SCENE 2
    m.knob(1, 1); // SCENE 3 (coalesced with the turn before: one undo step)
    m.knob(4, 1); // BPM "--" → the tempo in force
    m.f.settle();
    CHECK(m.project().arrangement.sections[0].scene == 2);
    REQUIRE(m.project().arrangement.sections[0].fixedBpm.has_value());
    CHECK(*m.project().arrangement.sections[0].fixedBpm == 120.0);
    CHECK(m.shell().column(0).valueText == "03");
    CHECK(m.shell().column(3).valueText == "120.0");
    m.session().undo(); // the BPM
    m.session().undo(); // both SCENE turns at once (EDT-U2)
    CHECK(m.project().arrangement.sections[0].scene == 0);
    // COPY row 1, PASTE before row 2 (never overwrites, UI-304) — via the rail and the keyboard.
    m.click(io::Control::ExtCopy);
    m.turnSel(1);
    m.click(io::Control::ExtPaste);
    m.f.settle();
    REQUIRE(rows(m) == 3);
    CHECK(m.project().arrangement.sections[1].repeats == 3);
    // DELETE (Delete key) on the focused row (row 3, the row the paste went before): a Toast, one undo step (UID-582).
    CHECK(ui::arrangerFocus(m.shell().body()) == 2);
    m.click(io::Control::ExtClear);
    m.f.settle();
    CHECK(rows(m) == 2);
    CHECK((m.logHas("TOAST Row 3 deleted") || m.hasText("Row 3 deleted")));
    m.click(io::Control::ExtUndo);
    CHECK(rows(m) == 3);
    // SHIFT + DELETE asks before clearing; CANCEL keeps the rows, DELETE ALL clears them.
    m.shiftClick(io::Control::ExtClear);
    m.f.settle();
    REQUIRE(m.shell().popupDepth() == 1);
    CHECK(m.hasText("Delete all 3 rows of the arrangement?"));
    m.click(io::Control::Back);
    m.f.settle();
    CHECK(rows(m) == 3);
    m.shiftClick(io::Control::ExtClear);
    m.turnSel(1); // DELETE ALL
    m.push();
    m.f.settle();
    CHECK(rows(m) == 0);
}

TEST_CASE("m5ui.arranger: playing row, follow rule, ARRANGEMENT toggle, SHIFT + push start, rail by rotary", "[m5]")
{
    M5Fixture m;
    auto& s = m.session();
    for (int i = 0; i < 14; ++i)
        s.execute(ar::appendRow(s.project(), row(1 + i % 3, 1, 1)));
    REQUIRE(m.open("arranger"));
    // ARRANGEMENT (rail 1) is reached by turning up from row 1: rail DELETE … ARRANGEMENT (NFR-040).
    m.turnSel(-1);
    CHECK(m.shell().railFocus() == 3);
    m.turnSel(-3);
    CHECK(m.shell().railFocus() == 0);
    m.push();
    m.f.settle();
    CHECK(m.project().settings.playArrangement);
    // Down past DELETE returns to row 1.
    m.turnSel(4);
    m.f.settle();
    CHECK(m.shell().railFocus() == -1);
    CHECK(ui::arrangerFocus(m.shell().body()) == 0);
    // The song plays row 12: the view follows it (the focus last moved more than 4 s ago).
    auto& song = core::arrangement::liveSongStatus();
    song.row = 11;
    song.remaining = 1;
    m.play(4.0);
    m.f.settle(6);
    CHECK(ui::arrangerFirstRow(m.shell().body()) > 0);
    CHECK(m.textColour("1 / 1") == ui::theme::palette(true).play);
    CHECK(m.state().sceneLocked); // ARR-007: SCENE | PATTERN greyed while the song plays
    // A focus move pins the view for 4 s.
    m.turnSel(1);
    m.f.frame();
    CHECK(ui::arrangerFirstRow(m.shell().body()) == 0);
    song.row = -1;
    // SHIFT + push (ARRANGEMENT on): the engine is told to start from the focused row (UI-305).
    m.f.press(io::Control::Shift);
    m.push();
    m.f.release(io::Control::Shift);
    CHECK(song.cued.load() == 1);
    CHECK(m.get("arrangement.start") == 1.0f);
}

TEST_CASE("m5ui.keyboard_only.arranger: build and play a song with the UI-120 keys alone (W10, UI-420)", "[m5]")
{
    M5Fixture m;
    m.jump(io::Control::ExtMixer, true, "arranger"); // SHIFT + ⌘1 → ARRANGER
    REQUIRE(m.shell().current().id == "arranger");
    m.push(); // Return: the current scene/pattern before END
    m.push();
    m.f.settle();
    REQUIRE(rows(m) == 2);
    m.turnSel(-1);    // ',' → row 2
    m.knob(2, 1);     // Ctrl+2, ↑: PATTERN 2
    m.knob(3, 3);     // Ctrl+3, ↑↑↑: REPEAT 4
    m.f.settle();
    CHECK(m.project().arrangement.sections[1].pattern == 1);
    CHECK(m.project().arrangement.sections[1].repeats == 4);
    m.click(io::Control::ExtCopy); // ⌘C, '.' to END, ⌘V: a third row
    m.turnSel(1);
    m.click(io::Control::ExtPaste);
    m.f.settle();
    REQUIRE(rows(m) == 3);
    m.turnSel(-5); // ',' up to the rail: ARRANGEMENT, Return
    while (m.shell().railFocus() != 0)
        m.turnSel(-1);
    m.push();
    CHECK(m.project().settings.playArrangement);
    // ARRANGEMENT on: SHIFT + Return plays from the focused row (control-surface.md SHIFT table); the shell's
    // SHIFT + push rail toggle applies only while ARRANGEMENT is off.
    m.turnSel(4); // back to row 1 (past the rail's end)
    REQUIRE(m.shell().railFocus() == -1);
    m.f.press(io::Control::Shift);
    m.push();
    m.f.release(io::Control::Shift);
    CHECK(m.shell().railFocus() == -1);
    CHECK(core::arrangement::liveSongStatus().cued.load() == ui::arrangerFocus(m.shell().body()));
    m.turnSel(-1); // ARRANGEMENT (rail)
    while (m.shell().railFocus() != 0)
        m.turnSel(-1);
    m.push();      // off
    m.turnSel(4);  // row 1
    m.f.press(io::Control::Shift);
    m.push();      // off: the rail toggle
    m.f.release(io::Control::Shift);
    CHECK(m.shell().railFocus() == 0);
    m.push();      // ARRANGEMENT on again
    m.click(io::Control::ExtUndo); // ⌘Z undoes the toggle, ⇧⌘Z redoes it — single steps
    CHECK_FALSE(m.project().settings.playArrangement);
    m.click(io::Control::ExtRedo);
    CHECK(m.project().settings.playArrangement);
}
