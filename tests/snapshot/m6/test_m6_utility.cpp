// UTILITY and its sub-pages (07-ui-design/screens/utility.md, UI-325…328, UID-875…887, SET-001…017):
// snapshots against utility.svg (editing PATTERN QUANTIZE) and channel-mapping.svg; every row of §Rows in order with
// its default; the editing state (push / turn / push, BACK restores, live preview for preferences, one undo step for
// project settings); long lists in a ListPopup; › pages and BACK to the same row; SHIFT + turn by section; CHANNEL
// MAPPING RESET's confirmation; info rows; a preference change reaching preferences.json within 1 s; every page by
// panel only (NFR-040); the registry.
#include "core/commands/ribbon/RibbonCommands.h"
#include "core/util/Realtime.h"
#include "services/settings/ProjectSettings.h"
#include "tests/snapshot/m6/M6Fixture.h"
#include "ui/shell/Layout.h"
#include "ui/widgets/ListPopup.h"
#include "ui/widgets/Message.h"

#include "ProjectIdentity.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace snapshot;
using m6test::M6Fixture;
namespace L = ui::layout;
namespace st = services::settings;

namespace
{
// The demo project of the mockups: PITCH RANGE ±2 (MIDI IN CLOCK and START/STOP: ENABLE, the defaults).
void mockupProject()
{
    auto& s = ui::m2::Session::instance();
    if (auto c = core::commands::ribbon::setPitchRange(s.project(), -200, 200))
        s.execute(std::move(c));
}
// Is the MIDI catalogue built in? (a function: the registry must not be built during static initialisation)
bool midiMerged() { return core::builtinParams().find("midi.syncSource") != nullptr; }
} // namespace

TEST_CASE("snapshot.m6.utility: UTILITY editing PATTERN QUANTIZE matches utility.svg", "[snapshot][m6]")
{
    Session session;
    M6Fixture t;
    mockupProject();
    REQUIRE(t.m.open("utility"));
    t.focusRow("PATTERN QUANTIZE");
    t.push();
    REQUIRE(t.editing());
    t.turn(-1); // PATTERN → BAR (shown in the cell; a project setting is applied on push)
    CHECK(t.value(t.focus()) == "BAR");
    const auto ours = t.m.f.image();
    const auto& mock = m6test::m6mockup("utility");
    CHECK(compare(ours, mock, L::canvas, "m6-utility").match >= kGate);
    CHECK(compare(ours, mock, {16, 152, 1248, 48}, "m6-utility-title").match >= kGate);
    CHECK(compare(ours, mock, {16, 200, 1240, 380}, "m6-utility-list").match >= kGate);
    CHECK(compare(ours, mock, {16, 368, 1240, 44}, "m6-utility-editing").match >= kGate);
    CHECK(compare(ours, mock, L::knobStrip, "m6-utility-strip").match >= kGate);
    CHECK(t.shell().stripTab() == "UTILITY");
    CHECK(t.m.hasText("6 sections"));
    CHECK(t.m.hasText("2 Bipolar Semitones"));
    CHECK(t.m.hasText("PROJECT SETTING (MIDI IN)"));
    t.back(); // BACK restores (nothing was applied)
    CHECK_FALSE(t.editing());
    CHECK(ui::m2::Session::instance().project().settings.patternQuantize == core::model::QuantizeUnit::Pattern);
}

TEST_CASE("snapshot.m6.utility_channel_mapping: CHANNEL MAPPING (MIDI IN) matches channel-mapping.svg",
          "[snapshot][m6]")
{
    Session session;
    M6Fixture t;
    REQUIRE(t.m.open("utility"));
    t.focusRow("CHANNEL MAPPING");
    t.push();
    REQUIRE(t.shell().current().id == "utility.channelMapIn");
    CHECK(t.focus() == 0);
    const auto ours = t.m.f.image();
    const auto& mock = m6test::m6mockup("channel-mapping");
    CHECK(compare(ours, mock, L::canvas, "m6-channel-mapping").match >= kGate);
    CHECK(compare(ours, mock, {16, 152, 1248, 448}, "m6-channel-mapping-list").match >= kGate);
    CHECK(t.m.hasText("track \xE2\x86\x90 receive channel"));
    CHECK(t.m.hasText("CH 1"));
    CHECK(t.m.hasText("TRACK 9"));
    CHECK(t.shell().stripTab() == "CHANNEL MAPPING");
}

TEST_CASE("m6.utility: every row of utility.md §Rows is present in 1.60 order with its default", "[m6]")
{
    M6Fixture t;
    REQUIRE(t.m.open("utility"));
    using K = ui::utility::SettingsRow::Kind;
    struct Want
    {
        const char* label;
        const char* value; // nullptr = not checked (sub-page rows, info)
        K kind;
    };
    const std::string sync = "INTERNAL";
    const Want want[] = {
        {"PROJECT SETTING (COMMON)", nullptr, K::Header},
        {"TOUCH STRIP SETTING (USER1)", nullptr, K::Action},
        {"TOUCH STRIP SETTING (USER2)", nullptr, K::Action},
        {"TOUCH STRIP SETTING (PITCH RANGE)", "2 Bipolar Semitones", K::Value},
        {"PATTERN QUANTIZE", "PATTERN", K::Value},
        {"PAD STARTS SEQUENCE", "OFF", K::Value},
        {"PROJECT SETTING (MIDI IN)", nullptr, K::Header},
        {"CLOCK", "ENABLE", K::Value},
        {"START/STOP/CONTINUE", "ENABLE", K::Value},
        {"NOTE/CC", "ENABLE", K::Value},
        {"CHANNEL MAPPING", nullptr, K::Action},
        {"CHANNEL MAPPING RESET", nullptr, K::Action},
        {"SONG POSITION", "ENABLE", K::Value},
        {"PROJECT SETTING (MIDI OUT)", nullptr, K::Header},
        {"CLOCK", "ENABLE", K::Value},
        {"START/STOP/CONTINUE", "ENABLE", K::Value},
        {"NOTE/CC", "ENABLE", K::Value},
        {"CHANNEL MAPPING RESET", nullptr, K::Action},
        {"SONG POSITION", "ENABLE", K::Value},
        {"CHANNEL MAPPING", nullptr, K::Action},
        {"MACHINE SETTING", nullptr, K::Header},
        {"SYNC SOURCE SELECT", sync.c_str(), K::Value},
        {"MIDI OUT MODE (DIN)", "OUT", K::Value}, // manual 1.60 p.12
        {"MIDI OUT MODE (USB)", "OUT", K::Value},
        {"MIDI PORTS", nullptr, K::Action},
        {"LINK SETTING", "START/STOP SYNC OFF", K::Value},
        {"PAD VELOCITY", "OFF", K::Value}, // requested default: strength → volume off
        {"PAD CURVE", "LINEAR", K::Value},
        {"PAD THRESHOLD", "5 (Default)", K::Value},
        {"PAD SENSITIVITY", "5 (Default)", K::Value},
        {"PAD MODE SELECT RESPONSE", "IMMEDIATELY", K::Value},
        {"METRONOME MONITOR", "Master + Headphones", K::Value},
        {"METRONOME VOLUME", "100", K::Value},
        {"COUNT IN", "OFF", K::Value},
        {"PAD/BUTTON BRIGHTNESS", "NORMAL", K::Value},
        {"AUDIO DEVICE", nullptr, K::Action},
        {"OUTPUT CHANNEL MAP", nullptr, K::Action},
        {"CONTROLLERS", nullptr, K::Action},
        {"KEYBOARD MAP", nullptr, K::Action},
        {"SCENE BPM", "ASK", K::Value},
        {"RESUME", "ON", K::Value},
        {"AUTOSAVE", "1 min", K::Value},
        {"THEME", "DARK", K::Value},
        {"UI SCALE", "100 %", K::Value},
        {"AMP ENV AUTO ZOOM", "OFF", K::Value}, // requested default: OFF
        {"CPU USAGE", "OFF", K::Value},
        // appended Sixteen rows, our own (UID-220)
        {"PATTERN SWITCH", nullptr, K::Value},
        {"SONG END", nullptr, K::Value},
        {"STRIP ACTS ON", "HELD PADS", K::Value},
        {"RESTART FX LFOS ON PLAY", nullptr, K::Value},
        {"SIXTEEN EXTENSIONS", "OFF", K::Value},
        {"SYSTEM", nullptr, K::Header},
        {"LIBRARY LOCATIONS", nullptr, K::Action},
        {"INFORMATION", nullptr, K::Header},
        {"Version No.", identity::version, K::Info},
        {"DISK SPACE", nullptr, K::Info},
    };
    const auto& rows = t.rows();
    REQUIRE(rows.size() == std::size(want));
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        INFO(i << " " << rows[i].label);
        CHECK(rows[i].label == want[i].label);
        CHECK(rows[i].kind == want[i].kind);
        if (want[i].value != nullptr)
            CHECK(t.value(static_cast<int>(i)) == want[i].value);
    }
    // INFORMATION is last; the focus starts on the first row (headers are not focusable).
    CHECK(t.focusedLabel() == "TOUCH STRIP SETTING (USER1)");
}

TEST_CASE("m6.utility: editing — push / turn / push commits, BACK restores, live preview for preferences", "[m6]")
{
    M6Fixture t;
    REQUIRE(t.m.open("utility"));
    const auto& reg = core::builtinParams();
    const auto* volume = reg.find("transport.metronomeVolume");
    t.focusRow("METRONOME VOLUME");
    t.push();
    REQUIRE(t.editing());
    t.turn(-5);
    CHECK(t.shell().values().get(*volume, 0) == 95.0f); // the preview is live (the click level follows)
    t.back();                                           // BACK: the value as it was
    CHECK_FALSE(t.editing());
    CHECK(t.shell().values().get(*volume, 0) == 100.0f);
    CHECK(t.shell().current().id == "utility"); // BACK in editing does not leave the screen
    t.push();
    t.turn(-10);
    t.push(); // commit
    CHECK_FALSE(t.editing());
    CHECK(t.shell().values().get(*volume, 0) == 90.0f);
    CHECK(t.prefs.get("transport.metronomeVolume") == 90.0f); // a machine setting: saved as a preference
    CHECK(t.value(t.focus()) == "90");
    // SHIFT + turn in editing = fine steps (integers stay 1).
    t.m.f.press(io::Control::Shift);
    t.push();
    t.turn(1);
    t.push();
    t.m.f.release(io::Control::Shift);
    CHECK(t.shell().values().get(*volume, 0) == 91.0f);
}

TEST_CASE("m6.utility: a project setting is applied on push as one undo step and marks the project edited", "[m6]")
{
    M6Fixture t;
    auto& s = ui::m2::Session::instance();
    REQUIRE(t.m.open("utility"));
    t.focusRow("PAD STARTS SEQUENCE");
    t.push();
    t.turn(1);
    CHECK_FALSE(s.project().settings.padStartsSequence); // shown, not applied yet
    CHECK(t.value(t.focus()) == "ON");
    t.push();
    CHECK(s.project().settings.padStartsSequence);
    CHECK(s.bus().dirty());
    CHECK(s.undo());
    CHECK_FALSE(s.project().settings.padStartsSequence);
    // PITCH RANGE: N Bipolar Semitones.
    t.focusRow("TOUCH STRIP SETTING (PITCH RANGE)");
    t.push();
    t.turn(-20); // to 1
    t.push();
    CHECK(s.project().ribbon.pitchTopCents == 100);
    CHECK(s.project().ribbon.pitchBottomCents == -100);
    // MIDI OUT NOTE/CC (the second NOTE/CC row).
    t.focusRow("NOTE/CC", 1);
    t.push();
    t.turn(-1);
    t.push();
    CHECK_FALSE(s.project().midi.output.noteCc);
    CHECK(s.project().midi.input.noteCc);
}

TEST_CASE("m6.utility: a list of more than eight values opens a ListPopup (UID-878)", "[m6]")
{
    M6Fixture t;
    auto& s = ui::m2::Session::instance();
    REQUIRE(t.m.open("utility.channelMapIn"));
    t.turn(2); // TRACK 3
    t.push();
    auto* list = dynamic_cast<ui::ListPopup*>(t.shell().topPopup());
    REQUIRE(list != nullptr);
    CHECK(list->rowCount() == 17); // OFF, CH 1 … CH 16
    CHECK(list->focusedItem() == 3); // the current value (CH 3)
    t.turn(4);
    t.push();
    CHECK(s.project().midi.input.channels[2] == 7);
    CHECK(t.value(2) == "CH 7");
    // PAD CURVE has 8 values: inline.
    REQUIRE(t.m.open("utility"));
    t.focusRow("PAD CURVE");
    t.push();
    CHECK(t.editing());
    CHECK(dynamic_cast<ui::ListPopup*>(t.shell().topPopup()) == nullptr);
    t.back();
}

TEST_CASE("m6.utility: › rows open their page, BACK returns to that row (UID-879), every page by panel only", "[m6]")
{
    M6Fixture t;
    REQUIRE(t.m.open("utility"));
    const struct
    {
        const char* row;
        int nth;
        const char* page;
        const char* title;
    } pages[] = {{"TOUCH STRIP SETTING (USER1)", 0, "utility.touchStrip.user1", "TOUCH STRIP SETTING (USER1)"},
                 {"TOUCH STRIP SETTING (USER2)", 0, "utility.touchStrip.user2", "TOUCH STRIP SETTING (USER2)"},
                 {"CHANNEL MAPPING", 0, "utility.channelMapIn", "CHANNEL MAPPING (MIDI IN)"},
                 {"CHANNEL MAPPING", 1, "utility.channelMapOut", "CHANNEL MAPPING (MIDI OUT)"},
                 {"MIDI PORTS", 0, "utility.midiPorts", "MIDI PORTS"},
                 {"AUDIO DEVICE", 0, "utility.audioDevice", "AUDIO DEVICE"},
                 {"OUTPUT CHANNEL MAP", 0, "utility.outputMap", "OUTPUT CHANNEL MAP"},
                 {"CONTROLLERS", 0, "utility.controllers", "CONTROLLERS"},
                 {"KEYBOARD MAP", 0, "utility.keyboardMap", "KEYBOARD MAP"},
                 {"LIBRARY LOCATIONS", 0, "utility.library", "LIBRARY LOCATIONS"}};
    for (const auto& p : pages)
    {
        INFO(p.page);
        t.focusRow(p.row, p.nth);
        const int row = t.focus();
        t.push();
        REQUIRE(t.shell().current().id == p.page);
        CHECK(t.shell().current().title == p.title);
        CHECK(t.shell().current().parent == "utility");
        t.back();
        REQUIRE(t.shell().current().id == "utility");
        CHECK(t.focus() == row);
    }
    // The NAV log names every page with its breadcrumb (HOME > UTILITY > <PAGE>).
    bool nav = false;
    for (const auto& line : t.m.f.log)
        nav = nav || line.find("utility.outputMap") != std::string::npos;
    CHECK(nav);
}

TEST_CASE("m6.utility: SHIFT + turn jumps by section, headers and disabled rows are skipped", "[m6]")
{
    M6Fixture t;
    REQUIRE(t.m.open("utility"));
    t.m.f.press(io::Control::Shift);
    t.turn(1);
    CHECK(t.focusedLabel() == "CLOCK"); // the first row of PROJECT SETTING (MIDI IN)
    t.turn(2);
    CHECK(t.focusedLabel() == (midiMerged() ? "SYNC SOURCE SELECT" : "MIDI PORTS"));
    t.turn(-1);
    CHECK(t.focusedLabel() == "CLOCK"); // MIDI OUT's first row
    t.m.f.release(io::Control::Shift);
    // LINK SETTING is disabled (SYNC SOURCE ≠ LINK): the rotary steps over it.
    t.focusRow("MIDI PORTS");
    t.turn(1);
    CHECK(t.focusedLabel() == "PAD VELOCITY");
    // The end: the rotary reaches the info rows; push there does nothing.
    t.turn(200);
    CHECK(t.focusedLabel() == "DISK SPACE");
    t.push();
    CHECK(t.shell().current().id == "utility");
    CHECK(t.shell().popupDepth() == 0);
}

TEST_CASE("m6.utility: CHANNEL MAPPING RESET asks first, RESET restores track n to channel n (SET-002)", "[m6]")
{
    M6Fixture t;
    auto& s = ui::m2::Session::instance();
    s.execute(st::setProjectValue(s.project(), *st::findProjectField("midi.in.channel"), 0, 9));
    REQUIRE(t.m.open("utility"));
    t.focusRow("CHANNEL MAPPING RESET");
    t.push();
    auto* msg = dynamic_cast<ui::Message*>(t.shell().topPopup());
    REQUIRE(msg != nullptr);
    CHECK(msg->focusedButton() == 0); // CANCEL
    t.push();                         // CANCEL: nothing changes
    CHECK(s.project().midi.input.channels[0] == 9);
    t.push();
    t.turn(1); // RESET
    t.push();
    CHECK(s.project().midi.input.channels[0] == 1);
    CHECK(s.undo());
    CHECK(s.project().midi.input.channels[0] == 9);
}

TEST_CASE("m6.utility: a preference change reaches preferences.json within 1 s", "[m6]")
{
    M6Fixture t;
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("m6-utility-" + juce::String(juce::Random::getSystemRandom().nextInt64()));
    REQUIRE(t.prefs.load(dir.getChildFile("preferences.json")));
    REQUIRE(t.m.open("utility"));
    t.prefs.tick(0);
    t.focusRow("RESUME");
    t.push();
    t.turn(-1); // OFF
    t.push();
    CHECK(t.prefs.get("project.resume") == 0.0f);
    t.prefs.tick(300);
    CHECK(t.prefs.writes() == 1);
    juce::var v;
    REQUIRE(juce::JSON::parse(dir.getChildFile("preferences.json").loadFileAsString(), v).wasOk());
    CHECK(static_cast<int>(v["values"]["project.resume"]) == 0);
    // THEME follows at once (UID-051).
    t.focusRow("THEME");
    t.push();
    t.turn(1);
    CHECK_FALSE(t.shell().state().dark);
    t.push();
    CHECK(t.prefs.get("ui.theme") == 1.0f);
    dir.deleteRecursively();
}

TEST_CASE("m6.utility: information rows and the providers (UI-328, UID-887)", "[m6]")
{
    M6Fixture t;
    auto& env = ui::utility::utilityEnv();
    env.diskSpace = [](std::function<void(double)> done) { done(212.4e9); };
    env.audioDevice = [] { return std::string("file:/tmp/main.wav"); };
    env.controllers = [] { return 2; };
    env.midiPorts = [] { return std::vector<std::string> {"DIN", "USB"}; };
    env.libraryLocations = [] {
        return std::vector<ui::utility::LibraryLocation> {{"LIBRARY ROOT", "/data", false}, {{}, "/media/usb", true}};
    };
    REQUIRE(t.m.open("utility"));
    CHECK(t.value(t.rowOf("DISK SPACE")) == "212.4 GB");
    CHECK(t.value(t.rowOf("AUDIO DEVICE")) == "file:/tmp/main.wav");
    CHECK(t.value(t.rowOf("CONTROLLERS")) == "2 connected");
    CHECK(t.value(t.rowOf("LIBRARY LOCATIONS")) == "2 locations");
    // MIDI PORTS: one group per port (IN, OUT MODE, CLOCK OFFSET).
    REQUIRE(t.m.open("utility.midiPorts"));
    CHECK(t.rowOf("DIN") >= 0);
    CHECK(t.rowOf("USB") >= 0);
    CHECK(t.rowOf("OUT MODE", 1) >= 0);
    // IN is a preference of its own (PrefSpec midi.portIn#p).
    t.focusRow("IN", 1);
    t.push();
    t.turn(-1);
    t.push();
    CHECK(t.prefs.get("midi.portIn#1") == 0.0f);
    // LIBRARY LOCATIONS: the root, the user's location (removable after a confirmation), ADD LOCATION….
    REQUIRE(t.m.open("utility.library"));
    CHECK(t.rows().front().label == "LIBRARY ROOT");
    CHECK(t.rows().back().label == "ADD LOCATION\xE2\x80\xA6");
    // SYNC SOURCE: the manual's values (a ListPopup only beyond eight values).
    if (midiMerged())
    {
        REQUIRE(t.m.open("utility"));
        t.focusRow("SYNC SOURCE SELECT");
        t.push();
        t.turn(2);
        CHECK(t.value(t.focus()) == "USB MIDI");
        t.push();
    }
}

TEST_CASE("m6.utility: MIDI OUT MODE (DIN) / (USB) and SYNC SOURCE SELECT as the manual has them (1.60 p.12-13)",
          "[m6]")
{
    if (! midiMerged())
        return;
    M6Fixture t;
    auto& env = ui::utility::utilityEnv();
    // The unit in USB storage mode: the USB port is absent (its slot keeps its name), a USB-host device on port 3.
    env.midiPorts = [] { return std::vector<std::string>{"MIDI DIN", "MIDI USB", "KEYS"}; };
    REQUIRE(t.m.open("utility"));
    // MIDI OUT MODE (DIN): five values on midi.portMode#0.
    t.focusRow("MIDI OUT MODE (DIN)");
    t.push();
    std::vector<std::string> seen{t.value(t.focus())};
    for (int i = 0; i < 5; ++i)
    {
        t.turn(1);
        seen.push_back(t.value(t.focus()));
    }
    CHECK(seen == std::vector<std::string>{"OUT", "OUT+MIDI IN", "OUT+USB IN", "THRU (MIDI IN)", "THRU (USB IN)",
                                           "THRU (USB IN)"});
    t.turn(-1);
    t.push();
    CHECK(t.prefs.get("midi.portMode") == 3.0f); // THRU (MIDI IN) = THRU A, the DIN input
    // MIDI OUT MODE (USB): OUT / OUT+MIDI IN only, on midi.portMode#1.
    t.focusRow("MIDI OUT MODE (USB)");
    t.push();
    t.turn(5);
    CHECK(t.value(t.focus()) == "OUT+MIDI IN");
    t.push();
    CHECK(t.prefs.get("midi.portMode#1") == 1.0f); // OUT + MERGE A = the DIN input
    CHECK(t.prefs.get("midi.portMode") == 3.0f);   // DIN's value stays its own
    CHECK(t.value(t.rowOf("MIDI OUT MODE (DIN)")) == "THRU (MIDI IN)");
    // A value the USB port cannot take shows as what it means.
    t.prefs.set("midi.portMode#1", 3); // THRU A (the DIN input) → OUT+MIDI IN
    CHECK(t.value(t.rowOf("MIDI OUT MODE (USB)")) == "OUT+MIDI IN");
    t.prefs.set("midi.portMode#1", 4); // THRU B (its own input) → OUT
    CHECK(t.value(t.rowOf("MIDI OUT MODE (USB)")) == "OUT");

    // SYNC SOURCE SELECT: INTERNAL / DIN MIDI / USB MIDI (SYNC PORT 1 / 2), then the further port by name.
    const auto* port = core::builtinParams().find("midi.syncPort");
    const auto* source = core::builtinParams().find("midi.syncSource");
    t.focusRow("SYNC SOURCE SELECT");
    t.push();
    seen = {t.value(t.focus())};
    for (int i = 0; i < 4; ++i)
    {
        t.turn(1);
        seen.push_back(t.value(t.focus()));
    }
    CHECK(seen ==
          std::vector<std::string>{"INTERNAL", "DIN MIDI", "USB MIDI", "MIDI CLOCK (KEYS)", "MIDI CLOCK (KEYS)"});
    t.turn(-1); // USB MIDI
    t.push();
    CHECK(t.shell().values().get(*source, 0) == 1.0f);
    CHECK(t.shell().values().get(*port, 0) == 2.0f);
    t.push();
    t.turn(1); // MIDI CLOCK (KEYS) = port 3
    t.push();
    CHECK(t.shell().values().get(*port, 0) == 3.0f);
    t.push();
    t.turn(-2); // DIN MIDI
    t.push();
    CHECK(t.shell().values().get(*port, 0) == 1.0f);
    CHECK(t.prefs.get("midi.syncPort") == 1.0f);

    // No port at all (a desktop without MIDI): the same three values; a SYNC PORT saved for an absent port still shows.
    env.midiPorts = [] { return std::vector<std::string>{}; };
    t.shell().values().set(*port, 0, 5.0f);
    REQUIRE(t.m.open("utility.midiPorts")); // the values are read on entry
    REQUIRE(t.m.open("utility"));
    CHECK(t.value(t.rowOf("SYNC SOURCE SELECT")) == "MIDI CLOCK (PORT 5)");
    t.focusRow("SYNC SOURCE SELECT");
    t.push();
    t.turn(-10);
    seen = {t.value(t.focus())};
    for (int i = 0; i < 3; ++i)
    {
        t.turn(1);
        seen.push_back(t.value(t.focus()));
    }
    CHECK(seen == std::vector<std::string>{"INTERNAL", "DIN MIDI", "USB MIDI", "MIDI CLOCK (PORT 5)"});
    t.back();

    // MIDI PORTS: DIN and USB offer the manual's values there too; a further port every mode.
    env.midiPorts = [] { return std::vector<std::string>{"MIDI DIN", "MIDI USB", "KEYS"}; };
    REQUIRE(t.m.open("utility.midiPorts"));
    t.focusRow("OUT MODE", 1);
    t.push();
    t.turn(5);
    CHECK(t.value(t.focus()) == "OUT+MIDI IN");
    t.back();
    t.focusRow("OUT MODE", 2);
    t.push();
    t.turn(5);
    CHECK(t.value(t.focus()) == "THRU B");
    t.back();
    env.midiPorts = nullptr;
}

TEST_CASE("m6.utility: touch — tap focuses, a tap on the focused row edits, ‹ › step", "[m6]")
{
    M6Fixture t;
    REQUIRE(t.m.open("utility"));
    // Row 5 of the view (y 412–456) = PAD STARTS SEQUENCE.
    t.m.f.tap(300, 430);
    t.m.f.settle();
    CHECK(t.focusedLabel() == "PAD STARTS SEQUENCE");
    t.m.f.tap(700, 430);
    t.m.f.settle();
    REQUIRE(t.editing());
    t.m.f.tap(1240, 434); // ›
    t.m.f.settle();
    CHECK(t.value(t.focus()) == "ON");
    t.m.f.tap(300, 250); // elsewhere: commit
    t.m.f.settle();
    CHECK_FALSE(t.editing());
    CHECK(ui::m2::Session::instance().project().settings.padStartsSequence);
}

TEST_CASE("m6.utility: the UTILITY screens validate (parents, bodies) and HOME cancels an edit", "[m6]")
{
    M6Fixture t;
    const auto problems = ui::ScreenRegistry::builtin().validate(ui::WidgetRegistry::builtin().dispatcher(),
                                                                 core::builtinParams());
    for (const auto& p : problems)
        CHECK((p.find("utility") == std::string::npos ? std::string() : p) == "");
    for (const char* id : {"utility", "utility.channelMapIn", "utility.channelMapOut", "utility.midiPorts",
                           "utility.audioDevice", "utility.outputMap", "utility.library"})
    {
        const auto* s = ui::ScreenRegistry::builtin().find(id);
        REQUIRE(s != nullptr);
        CHECK(s->bodyComponent == "SettingsList");
        CHECK(s->body == ui::BodyTemplate::Full);
        CHECK((std::string_view(id) == "utility" ? s->parent == "home" : s->parent == "utility"));
    }
    REQUIRE(t.m.open("utility"));
    const auto* volume = core::builtinParams().find("transport.metronomeVolume");
    t.focusRow("METRONOME VOLUME");
    t.push();
    t.turn(3);
    t.m.click(io::Control::Home);
    t.m.f.settle();
    CHECK(t.shell().current().id == "home");
    CHECK(t.shell().values().get(*volume, 0) == 100.0f);
    // SHIFT + HOME opens UTILITY (SET-014) with the focus remembered.
    t.m.f.press(io::Control::Shift);
    t.m.click(io::Control::Home);
    t.m.f.release(io::Control::Shift);
    t.m.f.settle();
    CHECK(t.shell().current().id == "utility");
    CHECK(t.focusedLabel() == "METRONOME VOLUME");
}

TEST_CASE("m6.utility: drawing UTILITY and moving its focus allocate nothing after warm-up", "[m6][alloc]")
{
    M6Fixture t(false); // no log sink (log lines are strings)
    auto& env = ui::utility::utilityEnv();
    env.diskSpace = [](std::function<void(double)> done) { done(1.5e9); };
    env.libraryLocations = [] { return std::vector<ui::utility::LibraryLocation> {{"LIBRARY ROOT", "/data", false}}; };
    REQUIRE(t.m.open("utility"));
    for (int i = 0; i < 3; ++i)
        t.m.f.frame(16);
    const auto before = core::rt::violationCount();
    core::rt::setAbortOnViolation(false);
    {
        core::rt::ScopedRealtimeThread rt;
        for (int i = 0; i < 60; ++i)
        {
            t.m.f.turn(io::Control::Selector, i < 30 ? 1 : -1);
            t.m.f.frame(16);
        }
    }
    core::rt::setAbortOnViolation(true);
    CHECK(core::rt::violationCount() - before == 0);
    CHECK(t.value(t.rowOf("DISK SPACE")) == "1.5 GB");
}
