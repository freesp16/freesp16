// The remappable keyboard map (control-surface.md UI-120 "all keys remappable"): the data table KEYBOARD
// MAP shows is the host KeyMap's behaviour, a remapped control moves to its new key (its old key goes quiet, a key
// drives one control), the KEYBOARD MAP capture takes the next key (Esc cancels, reserved keys refuse), Ctrl +
// backslash toggles the virtual panel, and the key codes mirrored in io/ equal the host's.
#include "io/controllers/profiles/KeyRemap.h"
#include "platform/host/input/KeyMap.h"

#include <catch2/catch_test_macros.hpp>

#include <string_view>
#include <vector>

namespace ctl = io::controllers;
namespace host = platform::host;
using io::Control;
using Kind = io::ControlEvent::Kind;

namespace
{
// The control a single key produces through `map` (Count = none).
Control press(host::KeyMap& map, int code, unsigned mods)
{
    std::vector<io::ControlEvent> out;
    map.key(code, mods, true, false, 0, nullptr, out);
    map.key(code, mods, false, false, 1, nullptr, out);
    for (const auto& e : out)
        if (e.kind == Kind::Press && e.control != Control::Shift)
            return e.control;
    return Control::Count;
}
unsigned modsOf(int cls)
{
    return cls == 2 ? host::kModCmd | host::kModShift : cls == 1 ? host::kModCmd : 0u;
}
} // namespace

TEST_CASE("the key codes mirrored in io equal the host KeyMap's", "[controllers]")
{
    CHECK(ctl::keys::f1 == host::kKeyF1);
    CHECK(ctl::keys::left == host::kKeyLeft);
    CHECK(ctl::keys::down == host::kKeyDown);
    CHECK(ctl::keys::ret == host::kKeyReturn);
    CHECK(ctl::keys::escape == host::kKeyEscape);
    CHECK(ctl::keys::home == host::kKeyHome);
    CHECK(ctl::keys::del == host::kKeyDelete);
    CHECK(ctl::keys::backspace == host::kKeyBackspace);
    CHECK(ctl::keys::modShift == host::kModShift);
    CHECK(ctl::keys::modCtrl == host::kModCtrl);
    CHECK(ctl::keys::modAlt == host::kModAlt);
    CHECK(ctl::keys::modCmd == host::kModCmd);
}

TEST_CASE("KEYBOARD MAP's default table is the UI-120 map the host KeyMap plays", "[controllers]")
{
    ctl::KeyRemap none;
    host::KeyMap map;
    map.setRemap(&none);
    for (const auto& b : ctl::defaultKeyBindings())
    {
        INFO(io::nameOf(b.control) << " key " << ctl::keyName(b.code, b.cls));
        const auto got = press(map, b.code, modsOf(b.cls));
        // ; and ' (NUDGE) are panel-key bindings: the table lists them; KeyMap may or may not map them (either passes).
        if (b.control == Control::ExtNudgeDown || b.control == Control::ExtNudgeUp)
            CHECK((got == b.control || got == Control::Count));
        else
            CHECK(got == b.control);
    }
    // Every plain printable key KeyMap maps is in the table.
    for (const char ch : std::string_view("abcdefghijklmnopqrstuvwxyz0123456789-=[]`;' "))
    {
        const int c = ch;
        const auto got = press(map, c, 0);
        if (got == Control::Count)
            continue;
        bool listed = false;
        for (const auto& b : ctl::defaultKeyBindings())
            listed = listed || (b.code == c && b.cls == 0 && b.control == got);
        INFO(static_cast<char>(c));
        CHECK(listed);
    }
}

TEST_CASE("a remapped control moves to its new key, a key drives one control", "[controllers]")
{
    ctl::KeyRemap remap;
    host::KeyMap map;
    map.setRemap(&remap);
    remap.assign(io::pad(1), 'h', 0);
    CHECK(press(map, 'h', 0) == io::pad(1));
    CHECK(press(map, 'z', 0) == Control::Count); // pad 1's default key goes quiet
    CHECK(remap.keyFor(io::pad(1)).code == 'h');
    remap.assign(Control::Play, 'x', 0); // x was pad 2's: now PLAY's, pad 2 has no key
    CHECK(press(map, 'x', 0) == Control::Play);
    CHECK(press(map, ' ', 0) == Control::Count);
    CHECK(remap.keyFor(io::pad(2)).code == 0);
    CHECK(ctl::keyName(remap.keyFor(io::pad(2)).code, 0) == "\xE2\x80\x94");
    remap.assign(Control::ExtUndo, 'u', 1);
    CHECK(press(map, 'u', host::kModCmd) == Control::ExtUndo);
    CHECK(press(map, 'z', host::kModCmd) == Control::Count);
    CHECK(press(map, 'u', 0) == io::step(11)); // the plain key is another key
    remap.reset(Control::Play);
    CHECK(press(map, ' ', 0) == Control::Play);
    CHECK(press(map, 'x', 0) == io::pad(2));
    remap.assign(io::pad(1), 'z', 0); // its own default = no override
    CHECK(remap.overrides().empty() == false); // ExtUndo's stays
    CHECK(press(map, 'z', 0) == io::pad(1));
    // Reserved keys are not remapped.
    CHECK_FALSE(remap.remappable(ctl::keys::up, 0));
    CHECK_FALSE(remap.remappable(',', 0));
    remap.assign(io::pad(3), ',', 0);
    CHECK(remap.keyFor(io::pad(3)).code == 'c');
    remap.resetAll();
    CHECK(remap.overrides().empty());
    CHECK(ctl::keyName('z', 2) == "\xE2\x87\xA7\xE2\x8C\x98Z");
    CHECK(ctl::keyName(ctl::keys::f1 + 4, 0) == "F5");
}

TEST_CASE("KEYBOARD MAP capture takes the next key, Esc cancels, Ctrl + backslash toggles the panel",
          "[controllers]")
{
    ctl::KeyRemap remap;
    host::KeyMap map;
    map.setRemap(&remap);
    remap.arm();
    CHECK(press(map, 'q', 0) == Control::Count); // captured, not played
    ctl::KeyBinding got;
    bool cancelled = true;
    REQUIRE(remap.takeCapture(got, cancelled));
    CHECK_FALSE(cancelled);
    CHECK(got.code == 'q');
    CHECK(press(map, 'q', 0) == io::pad(9)); // capture over
    remap.arm();
    press(map, host::kKeyEscape, 0);
    REQUIRE(remap.takeCapture(got, cancelled));
    CHECK(cancelled);
    remap.arm();
    press(map, host::kKeyUp, 0); // reserved: refused
    REQUIRE(remap.takeCapture(got, cancelled));
    CHECK(cancelled);
    const int before = remap.panelToggles();
    press(map, '\\', host::kModCtrl);
    CHECK(remap.panelToggles() == before + 1);
}
