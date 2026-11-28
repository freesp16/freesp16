// MIDI track screens (midi-track.md UID-800…811): MIDI NOTE, MIDI CC1 / CC2, SELECT CH and BANK SELECT
// against midi-note.svg, midi-cc1.svg, midi-cc2.svg, midi-select-ch.svg, midi-bank-select.svg (track 10), and the
// keyboard-only MIDI track setup (UI-420). The values are the midi.* parameters: while that catalogue is not
// registered (it is built separately) the bodies draw its defaults and the value checks are skipped with a note.
#include "core/params/ParamRegistry.h"
#include "tests/snapshot/m5/M5Fixture.h"
#include "ui/shell/Layout.h"
#include "ui/widgets/CcCard.h"
#include "ui/widgets/ListPopup.h"
#include "ui/widgets/PianoKeyboard.h"
#include "ui/widgets/SettingPopup.h"

#include <catch2/catch_test_macros.hpp>

using namespace snapshot;
using m5test::M5Fixture;
namespace L = ui::layout;

namespace ui
{
int midiCcFocus(const Widget* body) noexcept; // ui/screens/miditrack/MidiTrackScreen.cpp
}

namespace
{
bool haveMidi()
{
    return core::builtinParams().find("midi.note1") != nullptr;
}
struct MidiFixture : M5Fixture
{
    MidiFixture()
    {
        set("track.mode", 10, 1); // MIDI
        auto& t = state().tracks[9];
        t.typeBadge = "MIDI";
        t.name = "MIDI CH.10";
        t.hasContent = true;
        t.stretchBadge = {};
        t.loopGlyph = {};
        shell().selectTrack(10);
        // The model's default send channel of track n is n (our choice); the fixture's local values start at 1.
        if (const auto* ch = core::builtinParams().find("midi.out.channel"))
            shell().values().set(*ch, 9, 10.0f);
    }
};
} // namespace

TEST_CASE("snapshot.m5.midi_note: MIDI NOTE (track 10, NOTE with NOTE2 +7) matches midi-note.svg", "[snapshot][m5]")
{
    Session session;
    MidiFixture m;
    if (haveMidi())
    {
        m.set("midi.note1", 10, 60); // C5: the mockup's NOTE key (the manual's naming, 0 = C0)
        m.set("midi.note2", 10, 7);  // +7 semitones: G5
    }
    REQUIRE(m.open("track.menu"));
    REQUIRE(m.open("track.note"));
    const auto ours = m.f.image();
    CHECK(m.shell().current().parent == "track.menu");
    const auto& mock = m5test::m5mockup("midi-note");
    CHECK(compare(ours, mock, {16, 152, 632, 340}, "m5-midi-note-band1").match >= kGate);
    CHECK(compare(ours, mock, L::contextBar, "m5-midi-note-context").match >= kGate);
    if (haveMidi()) // NOTE2's key (+7, 60 %) needs midi.note2
        CHECK(compare(ours, mock, {16, 152, 1248, 340}, "m5-midi-note-keys").match >= kGate);
    if (! haveMidi())
        SKIP("midi.* not registered (no MIDI catalogue built in): overview window, strip and canvas need its values");
    CHECK(m.hasText("+7"));
    CHECK(compare(ours, mock, {16, 500, 1248, 108}, "m5-midi-note-overview").match >= kGate);
    CHECK(compare(ours, mock, L::canvas, "m5-midi-note").match >= kGate);
}

TEST_CASE("snapshot.m5.midi_cc: MIDI CC1 / CC2 (card 3 focused) match midi-cc1.svg / midi-cc2.svg", "[snapshot][m5]")
{
    Session session;
    for (int page = 1; page <= 2; ++page)
    {
        MidiFixture m;
        if (haveMidi())
        {
            if (page == 1)
            {
                m.set("midi.cc1.active3", 10, 1);
                m.set("midi.cc1.active6", 10, 1);
                m.set("midi.cc1.value3", 10, 100);
                m.set("midi.cc1.value6", 10, 64);
            }
            else
            {
                m.set("midi.cc2.active1", 10, 1);
                m.set("midi.cc2.active4", 10, 1);
                for (int i = 1; i <= 6; ++i)
                    m.set("midi.cc2.value" + std::to_string(i), 10, i == 4 ? 80.0f : 64.0f);
            }
        }
        REQUIRE(m.open("track.menu"));
        REQUIRE(m.open(page == 1 ? "track.cc1" : "track.cc2"));
        m.turnSel(2); // card 3
        const auto ours = m.f.image();
        CHECK(ui::midiCcFocus(m.shell().body()) == 2);
        CHECK(m.hasText(page == 1 ? "CC #7" : "CC #73"));
        CHECK(m.hasText(page == 1 ? "Channel" : "Attack"));
        const auto& mock = m5test::m5mockup(page == 1 ? "midi-cc1" : "midi-cc2");
        const std::string name = page == 1 ? "m5-midi-cc1" : "m5-midi-cc2";
        CHECK(compare(ours, mock, {16, 140, 1248, 448}, name + "-cards").match >= kGate);
        if (haveMidi())
        {
            CHECK(compare(ours, mock, L::knobStrip, name + "-strip").match >= kGate);
            CHECK(compare(ours, mock, L::canvas, name).match >= kGate);
        }
    }
}

TEST_CASE("snapshot.m5.midi_popups: SELECT CH and BANK SELECT over TRACK MENU (MIDI) match their mockups",
          "[snapshot][m5]")
{
    Session session;
    {
        MidiFixture m;
        REQUIRE(m.open("track.menu"));
        m.push(); // SELECT CH holds the focus on a MIDI track's TRACK MENU
        m.f.settle();
        auto* list = dynamic_cast<ui::ListPopup*>(m.shell().topPopup());
        REQUIRE(list != nullptr);
        CHECK(list->rowCount() == 16);
        CHECK(list->focusedItem() == 9); // CH.10 (default channel n = track n)
        m.turnSel(2); // the mockup's scroll position: CH.6 … CH.13, the focus back on CH.10
        m.turnSel(-2);
        const auto ours = m.f.image();
        const auto& mock = m5test::m5mockup("midi-select-ch");
        CHECK(compare(ours, mock, list->bounds(), "m5-midi-select-ch-popup").match >= kGate);
        CHECK(compare(ours, mock, L::canvas, "m5-midi-select-ch").match >= kGate);
    }
    {
        MidiFixture m;
        if (haveMidi())
        {
            m.set("midi.bankMsb", 10, 0);
            m.set("midi.bankLsb", 10, 0);
        }
        REQUIRE(m.open("track.menu"));
        m.turnSel(1); // BANK SELECT
        m.push();
        m.f.settle();
        auto* setting = dynamic_cast<ui::SettingPopup*>(m.shell().topPopup());
        REQUIRE(setting != nullptr);
        CHECK(m.hasText("BANK SELECT \xC2\xB7 SCENE 01"));
        const auto ours = m.f.image();
        const auto& mock = m5test::m5mockup("midi-bank-select");
        CHECK(compare(ours, mock, setting->bounds(), "m5-midi-bank-popup").match >= kGate);
        if (haveMidi())
            CHECK(compare(ours, mock, L::canvas, "m5-midi-bank-select").match >= kGate);
    }
}

TEST_CASE("m5ui.midi: chord notes by SHIFT + tap, NOTE2 offsets, CC ACTIVE / CC NUMBER, the window", "[m5]")
{
    MidiFixture m;
    REQUIRE(m.open("track.note"));
    // A tap on a key sets NOTE (no catalogue: nothing to set, the tap is still taken).
    const float x = ui::PianoKeyboard::whiteX(9) + 40; // the right band's E
    m.f.tap(x, 300);
    if (haveMidi())
    {
        CHECK(m.get("midi.note1", 10) == 64.0f);
        m.f.press(io::Control::Shift);
        m.f.tap(ui::PianoKeyboard::whiteX(11) + 40, 300); // G: the first free chord note
        m.f.release(io::Control::Shift);
        CHECK(m.get("midi.note2", 10) == 3.0f); // G = NOTE (E) + 3
        m.f.settle();
        CHECK(m.hasText("+3")); // NOTE2 drawn by its ParamSpec format: semitones from NOTE
        m.knob(4, -3); // NOTE2 offset +3 → 0 = OFF
        CHECK(m.get("midi.note2", 10) == 0.0f);
    }
    m.turnSel(1); // the window one octave up
    m.f.settle();
    REQUIRE(m.open("track.cc1"));
    m.turnSel(3);
    m.push(); // ACTIVE of card 4 (CC #1)
    if (haveMidi())
        CHECK(m.get("midi.cc1.active4", 10) == 1.0f);
    m.f.press(io::Control::Shift);
    m.push(); // CC NUMBER list
    m.f.release(io::Control::Shift);
    m.f.settle();
    auto* list = dynamic_cast<ui::ListPopup*>(m.shell().topPopup());
    REQUIRE(list != nullptr);
    CHECK(list->rowCount() == 128);
    CHECK(list->focusedItem() == 1); // CC #1
    m.turnSel(10);
    m.push();
    if (haveMidi())
        CHECK(m.get("midi.cc1.cc4", 10) == 11.0f);
    std::string_view a, b;
    ui::CcCard::nameOf(74, a, b);
    CHECK(a == "Brightness");
}

TEST_CASE("m5ui.midi: the keyboard names octaves C0…C9 (MIDI 0 = C0, 60 = C5, 119 = B9)", "[m5]")
{
    ui::PianoKeyboardModel k;
    k.firstOctave = 0;
    CHECK(ui::PianoKeyboard::noteAt(k, ui::PianoKeyboard::whiteX(0) + 40, 300) == 0);   // C0
    CHECK(ui::PianoKeyboard::noteAt(k, ui::PianoKeyboard::whiteX(13) + 40, 300) == 23); // B1
    k.firstOctave = 8;
    CHECK(ui::PianoKeyboard::noteAt(k, ui::PianoKeyboard::whiteX(13) + 40, 300) == 119); // B9, the highest NOTE
    MidiFixture m;
    if (! haveMidi())
        SKIP("midi.* not registered (no MIDI catalogue built in)");
    m.set("midi.note1", 10, 119);
    REQUIRE(m.open("track.note"));
    m.f.settle();
    CHECK(m.hasText("B9")); // the window follows NOTE up to the C8 / C9 bands
    CHECK(m.hasText("C8"));
    m.f.tap(ui::PianoKeyboard::whiteX(7) + 40, 300); // the right band's C
    CHECK(m.get("midi.note1", 10) == 108.0f);         // C9
    m.knob(1, 20);
    CHECK(m.get("midi.note1", 10) == 119.0f); // NOTE stops at B9
}

TEST_CASE("m5ui.midi: TRACK TYPE names the AS-1 type TORAIZ AS-1 (the manual, p.40) and it fits its box", "[m5]")
{
    MidiFixture m;
    m.set("track.mode", 10, 3); // TORAIZ AS-1
    m.state().tracks[9].typeBadge = "AS-1";
    REQUIRE(m.open("track.menu"));
    CHECK(m.hasText("TORAIZ AS-1")); // the context bar's TRACK TYPE value
    const float w = measure()(ui::TextStyleId::ValueS, "TORAIZ AS-1");
    CHECK(w <= L::trackTypeBox.w - 32); // centred with room on both sides: never clipped
    m.f.tap(L::trackTypeBox.x + L::trackTypeBox.w / 2, L::trackTypeBox.y + L::trackTypeBox.h / 2);
    m.f.settle();
    auto* list = dynamic_cast<ui::ListPopup*>(m.shell().topPopup());
    REQUIRE(list != nullptr);
    CHECK(list->rowCount() == 5); // SAMPLE, THRU, MIDI, TORAIZ AS-1, PLUGIN
    CHECK(list->focusedItem() == 3);
    CHECK(m.hasText("TORAIZ AS-1"));
}

TEST_CASE("m5ui.midi: an AS-1 track picks its bank U1…F5 and program P01…P99 per scene (the manual's 7.1)", "[m5]")
{
    MidiFixture m;
    m.set("track.mode", 10, 3); // TORAIZ AS-1
    m.state().tracks[9].typeBadge = "AS-1";
    REQUIRE(m.open("track.menu"));
    if (! haveMidi())
        SKIP("midi.* not registered (no MIDI catalogue built in)");
    CHECK(m.hasText("---")); // nothing stored yet: neither bank nor program is sent
    m.turnSel(1);            // SELECT CH → BANK SELECT
    m.push();
    m.f.settle();
    auto* list = dynamic_cast<ui::ListPopup*>(m.shell().topPopup());
    REQUIRE(list != nullptr);
    CHECK(list->rowCount() == 10);
    CHECK(m.hasText("BANK SELECT \xC2\xB7 SCENE 01"));
    CHECK(m.hasText("U1"));
    CHECK(m.hasText("F1")); // the list's first rows (it scrolls to F5)
    m.turnSel(6); // U1 → F2
    m.push();     // the push fixes the bank
    m.f.settle();
    CHECK(m.shell().popupDepth() == 0);
    CHECK(m.get("midi.bankMsb", 10) == 0.0f); // UNVERIFIED mapping: MSB 0, LSB 5–9 = F1–F5
    CHECK(m.get("midi.bankLsb", 10) == 6.0f);
    CHECK(m.hasText("F2")); // the button shows the stored bank
    m.turnSel(1);           // PROG CHANGE
    m.push();
    m.f.settle();
    list = dynamic_cast<ui::ListPopup*>(m.shell().topPopup());
    REQUIRE(list != nullptr);
    CHECK(list->rowCount() == 99);
    m.turnSel(11); // P01 → P12
    m.push();
    m.f.settle();
    CHECK(m.get("midi.program", 10) == 11.0f); // P12 = Program Change 11
    CHECK(m.hasText("P12"));
    // The picker opens on the stored value; another bank's numbers (a MIDI track's) show as none.
    m.turnSel(-1);
    m.push();
    m.f.settle();
    list = dynamic_cast<ui::ListPopup*>(m.shell().topPopup());
    REQUIRE(list != nullptr);
    CHECK(list->focusedItem() == 6);
    m.click(io::Control::Back);
    m.f.settle();
    m.set("midi.bankLsb", 10, 40);
    m.f.settle();
    CHECK_FALSE(m.hasText("F2"));
    CHECK(m.hasText("---"));
    // Per scene: scene 2 keeps its own (unset) values.
    m.state().scene = 2;
    m.f.settle();
    CHECK(m.hasText("---"));
    CHECK_FALSE(m.hasText("P12"));
}

TEST_CASE("m5ui.midi: an AS-1 track has no NOTE2-4 (no chords, manual p.41): blank knobs, no SHIFT + tap, no keys",
          "[m5]")
{
    MidiFixture m;
    m.set("track.mode", 10, 3); // TORAIZ AS-1
    m.state().tracks[9].typeBadge = "AS-1";
    if (haveMidi())
    {
        m.set("midi.note1", 10, 64);
        m.set("midi.note2", 10, 7); // left from when it was a MIDI track: the AS-1 does not play it
    }
    REQUIRE(m.open("track.note"));
    m.f.settle();
    CHECK_FALSE(m.hasText("+7")); // neither the strip nor the keyboard shows the chord note
    m.f.press(io::Control::Shift);
    m.f.tap(ui::PianoKeyboard::whiteX(11) + 40, 300); // SHIFT + tap on G: no chord note
    m.f.release(io::Control::Shift);
    m.knob(5, 3); // NOTE3's column is blank
    m.knob(6, 3);
    if (haveMidi())
    {
        CHECK(m.get("midi.note2", 10) == 7.0f);
        CHECK(m.get("midi.note3", 10) == 0.0f);
        CHECK(m.get("midi.note4", 10) == 0.0f);
        CHECK(m.get("midi.note1", 10) == 64.0f);
    }
    // Back to a MIDI track: the chord columns and keys return.
    m.set("track.mode", 10, 1);
    m.state().tracks[9].typeBadge = "MIDI";
    m.f.settle();
    m.knob(5, 3);
    if (haveMidi())
    {
        CHECK(m.get("midi.note3", 10) == 3.0f);
        m.f.settle();
        CHECK(m.hasText("+7"));
    }
}

TEST_CASE("m5ui.keyboard_only.midi_track: set up a MIDI track with the UI-120 keys alone (W-MIDI, UI-420)", "[m5]")
{
    MidiFixture m;
    m.push(); // HOME (track 10) → TRACK MENU, SELECT CH focused
    m.f.settle();
    REQUIRE(m.shell().current().id == "track.menu");
    m.push(); // Return: SELECT CH
    m.f.settle();
    REQUIRE(m.shell().popupDepth() == 1);
    m.turnSel(2); // CH.12
    m.push();
    m.f.settle();
    CHECK(m.shell().popupDepth() == 0);
    if (haveMidi())
        CHECK(m.shell().values().get(*core::builtinParams().find("midi.out.channel"), 9) == 12.0f);
    m.turnSel(1); // BANK SELECT
    m.push();
    m.f.settle();
    REQUIRE(m.shell().popupDepth() == 1);
    m.push();     // BANK MSB: the nested list (--- … 127)
    m.f.settle();
    m.turnSel(5); // --- → 004
    m.push();
    if (haveMidi())
        CHECK(m.get("midi.bankMsb", 10) == 4.0f);
    m.click(io::Control::Back);
    m.f.settle();
    m.turnSel(3); // PROG CHANGE → SET SCALE → NOTE card
    m.push();
    m.f.settle();
    REQUIRE(m.shell().current().id == "track.note");
    m.knob(1, 7); // Ctrl+1 ↑ ×7: NOTE G5
    if (haveMidi())
        CHECK(m.get("midi.note1", 10) == 67.0f);
    m.click(io::Control::Back);
    m.f.settle();
    m.turnSel(1); // CC1
    m.push();
    m.f.settle();
    REQUIRE(m.shell().current().id == "track.cc1");
    m.turnSel(2);
    m.push(); // ACTIVE on CC #7
    m.knob(3, 20);
    if (haveMidi())
    {
        CHECK(m.get("midi.cc1.active3", 10) == 1.0f);
        CHECK(m.get("midi.cc1.value3", 10) == 20.0f);
    }
}
