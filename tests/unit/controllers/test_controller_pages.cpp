// UTILITY › CONTROLLERS and KEYBOARD MAP (screens.md UI-330, control-surface.md UI-120): both screens are
// registered under the frozen ids, CONTROLLERS lists a port with its rows, edits the pad response, switches the role
// and walks MIDI learn up to the captured message; KEYBOARD MAP waits for a key and moves the control to it.
#include "tests/unit/ui/ShellHarness.h"

#include "io/controllers/profiles/ControllerHub.h"
#include "io/controllers/profiles/KeyRemap.h"
#include "platform/host/input/KeyMap.h"

#include <catch2/catch_test_macros.hpp>

namespace ctl = io::controllers;
using io::Control;

namespace
{
uitest::Harness pages()
{
    auto& reg = ui::ScreenRegistry::builtin();
    const auto* utility = reg.find("utility");
    const auto* controllers = reg.find("utility.controllers");
    const auto* keyboard = reg.find("utility.keyboardMap");
    REQUIRE(utility != nullptr);
    REQUIRE(controllers != nullptr);
    REQUIRE(keyboard != nullptr);
    return uitest::Harness({*utility, *controllers, *keyboard});
}

core::midi::MidiEvent ev(std::uint8_t a, std::uint8_t b, std::uint8_t c)
{
    core::midi::MidiEvent e;
    e.size = 3;
    e.bytes[0] = a;
    e.bytes[1] = b;
    e.bytes[2] = c;
    return e;
}
} // namespace

TEST_CASE("the CONTROLLERS and KEYBOARD MAP screens are registered under UTILITY", "[controllers]")
{
    auto& reg = ui::ScreenRegistry::builtin();
    for (const auto* id : {"utility.controllers", "utility.keyboardMap"})
    {
        const auto* s = reg.find(id);
        REQUIRE(s != nullptr);
        CHECK(s->parent == "utility");
    }
}

TEST_CASE("CONTROLLERS: a port's rows, pad response editing, role and MIDI learn", "[controllers]")
{
    auto& hub = ctl::hub();
    hub.log = [](const std::string&) {};
    hub.inputOpened(6, "PAGE PADS");
    hub.setRole("PAGE PADS", ctl::PortRole::Controller);
    REQUIRE(hub.source("PAGE PADS") != nullptr);
    auto h = pages();
    REQUIRE(h.shell->open("utility.controllers"));
    h.frame();
    CHECK(h.drewText("CONTROLLERS"));
    CHECK(h.drewText("PAGE PADS"));
    CHECK(h.drewText("Generic 16 + 8 (AUTO)"));
    CHECK(h.drewText("LINEAR"));
    CHECK(h.drewText("CONTROLLER"));
    // VELOCITY TEST shows the last hit live.
    CHECK(hub.message(6, ev(0x99, 36, 93)));
    h.frame();
    CHECK(h.drewText("93"));
    // Focus starts on ROLE; PAD CURVE is two rows down: push = edit, turn = step, push = done.
    h.turn(Control::Selector, 2);
    h.tap(Control::SelectorPush);
    h.turn(Control::Selector, -3);
    h.tap(Control::SelectorPush);
    h.frame();
    CHECK(h.drewText("HARD 3"));
    for (const auto& p : hub.ports())
        if (p.name == "PAGE PADS")
            CHECK(p.settings.response.shape == io::PadCurveShape::Hard3);
    // MIDI LEARN (the last row of the port): move a control, the message shows.
    h.turn(Control::Selector, 6);
    h.tap(Control::SelectorPush);
    CHECK(hub.learnState() == ctl::LearnState::Waiting);
    h.frame();
    CHECK(h.drewText("Move a control\xE2\x80\xA6"));
    CHECK(hub.message(6, ev(0x92, 70, 100)));
    h.frame();
    CHECK(hub.learnState() == ctl::LearnState::Captured);
    CHECK(h.drewText("NOTE 70  CH 3"));
    h.turn(Control::Selector, -5); // the "‹ CONTROLLERS" row (BACK would leave the page and end learn too)
    h.tap(Control::SelectorPush);
    h.frame();
    CHECK(hub.learnState() == ctl::LearnState::Idle);
    CHECK(h.drewText("PAD CURVE"));
    // ROLE cycles CONTROLLER → INSTRUMENT: the port stops being a controller.
    h.turn(Control::Selector, -20);
    h.tap(Control::SelectorPush);
    CHECK(hub.source("PAGE PADS") == nullptr);
    hub.setRole("PAGE PADS", ctl::PortRole::Auto);
    hub.inputClosed(6);
}

TEST_CASE("KEYBOARD MAP: push waits for a key, the control moves to it, SHIFT + push resets", "[controllers]")
{
    auto& remap = ctl::keyRemap();
    remap.resetAll();
    auto h = pages();
    REQUIRE(h.shell->open("utility.keyboardMap"));
    h.frame();
    CHECK(h.drewText("KEYBOARD MAP"));
    CHECK(h.drewText("pad.1"));
    CHECK(h.drewText("Z"));
    h.tap(Control::SelectorPush); // the first row: pad.1
    CHECK(remap.armed());
    h.frame();
    CHECK(h.drewText("Press a key\xE2\x80\xA6"));
    platform::host::KeyMap keys; // the app's keyboard (the global remap)
    std::vector<io::ControlEvent> out;
    CHECK(keys.key('h', 0, true, false, 0, nullptr, out)); // captured
    CHECK(out.empty());
    h.frame();
    CHECK(remap.keyFor(io::pad(1)).code == 'h');
    CHECK(h.drewText("H"));
    keys.key('h', 0, false, false, 1, nullptr, out);
    out.clear();
    keys.key('h', 0, true, false, 2, nullptr, out);
    REQUIRE_FALSE(out.empty());
    CHECK(out.back().control == io::pad(1));
    keys.key('h', 0, false, false, 3, nullptr, out);
    // Esc cancels a capture.
    h.tap(Control::SelectorPush);
    out.clear();
    keys.key(platform::host::kKeyEscape, 0, true, false, 4, nullptr, out);
    h.frame();
    CHECK(remap.keyFor(io::pad(1)).code == 'h');
    // SHIFT + push = back to the default key.
    h.press(Control::Shift);
    h.tap(Control::SelectorPush);
    h.release(Control::Shift);
    CHECK(remap.keyFor(io::pad(1)).code == 'z');
    remap.resetAll();
}
