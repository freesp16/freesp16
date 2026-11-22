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
        m.set("midi.note1", 10, 72);
        m.set("midi.note2", 10, 79);
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
        CHECK(m.get("midi.note2", 10) == 67.0f);
        m.f.settle();
        CHECK(m.shell().column(3).valueText == "+3");
        m.knob(4, -3); // NOTE2 offset +3 → 0 = OFF
        CHECK(m.get("midi.note2", 10) == -1.0f);
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
    m.knob(1, 7); // Ctrl+1 ↑ ×7: NOTE G4
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
