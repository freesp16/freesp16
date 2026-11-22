// The linux window's input mapping: X keysyms → the UI-120 KeyMap codes, X modifier state, and window
// pixels → logical 800 × 480 touch points at every scale. Pure functions: no X server needed.
#if defined(__linux__)
#include "platform/host/input/KeyMap.h"
#include "platform/linux/input/PointerMap.h"
#include "platform/linux/input/XKeyMap.h"

#include <X11/X.h>
#include <X11/keysym.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace platform::host;
using namespace platform::linux_desktop;

TEST_CASE("platform linux: X keysyms map to the KeyMap codes", "[platform][linux]")
{
    CHECK(keyCodeForKeysym(XK_a) == 'a');
    CHECK(keyCodeForKeysym(XK_A) == 'a');
    CHECK(keyCodeForKeysym(XK_z) == 'z');
    CHECK(keyCodeForKeysym(XK_1) == '1');
    CHECK(keyCodeForKeysym(XK_minus) == '-');
    CHECK(keyCodeForKeysym(XK_bracketleft) == '[');
    CHECK(keyCodeForKeysym(XK_grave) == '`');
    CHECK(keyCodeForKeysym(XK_space) == ' ');
    CHECK(keyCodeForKeysym(XK_F1) == kKeyF1);
    CHECK(keyCodeForKeysym(XK_F12) == kKeyF1 + 11);
    CHECK(keyCodeForKeysym(XK_Left) == kKeyLeft);
    CHECK(keyCodeForKeysym(XK_Right) == kKeyRight);
    CHECK(keyCodeForKeysym(XK_Up) == kKeyUp);
    CHECK(keyCodeForKeysym(XK_Down) == kKeyDown);
    CHECK(keyCodeForKeysym(XK_Return) == kKeyReturn);
    CHECK(keyCodeForKeysym(XK_KP_Enter) == kKeyReturn);
    CHECK(keyCodeForKeysym(XK_Escape) == kKeyEscape);
    CHECK(keyCodeForKeysym(XK_Home) == kKeyHome);
    CHECK(keyCodeForKeysym(XK_Delete) == kKeyDelete);
    CHECK(keyCodeForKeysym(XK_BackSpace) == kKeyBackspace);
    CHECK(keyCodeForKeysym(XK_Shift_L) == 0);
    CHECK(keyCodeForKeysym(XK_Tab) == 0);
    CHECK(keyCodeForKeysym(XK_eacute) == 0);

    CHECK(modsForState(0) == 0);
    CHECK(modsForState(ShiftMask) == kModShift);
    CHECK(modsForState(ControlMask | Mod1Mask) == (kModCtrl | kModAlt));
    CHECK(modsForState(Mod4Mask | LockMask | Mod2Mask) == kModCmd); // Caps/Num Lock ignored
    CHECK(modifierForKeysym(XK_Shift_R) == kModShift);
    CHECK(modifierForKeysym(XK_Control_L) == kModCtrl);
    CHECK(modifierForKeysym(XK_Alt_L) == kModAlt);
    CHECK(modifierForKeysym(XK_Super_L) == kModCmd);
    CHECK(modifierForKeysym(XK_a) == 0);

    // Through the UI-120 map: the a key is pad 5, z pad 1, space play.
    KeyMap map;
    std::vector<io::ControlEvent> out;
    map.key(keyCodeForKeysym(XK_a), 0, true, false, 0, nullptr, out);
    map.key(keyCodeForKeysym(XK_space), 0, true, false, 0, nullptr, out);
    REQUIRE(out.size() == 2);
    CHECK(out[0].control == io::pad(5));
    CHECK(out[1].control == io::Control::Play);
}

TEST_CASE("platform linux: window pixels → logical touch points", "[platform][linux]")
{
    CHECK(toLogicalPoint(0, 0, 1).x == 0);
    CHECK(toLogicalPoint(400, 240, 1).x == 400);
    CHECK(toLogicalPoint(400, 240, 1).y == 240);
    CHECK(toLogicalPoint(799, 479, 1).x == 799);
    CHECK(toLogicalPoint(800, 480, 1).x == 799); // clamped: a drag may leave the window
    CHECK(toLogicalPoint(800, 480, 1).y == 479);
    CHECK(toLogicalPoint(-5, -1, 1).x == 0);
    CHECK(toLogicalPoint(-5, -1, 1).y == 0);
    CHECK(toLogicalPoint(801, 481, 2).x == 400); // scale 2: 1600 x 960 window
    CHECK(toLogicalPoint(801, 481, 2).y == 240);
    CHECK(toLogicalPoint(1599, 959, 2).x == 799);
    CHECK(toLogicalPoint(1599, 959, 2).y == 479);
    CHECK(toLogicalPoint(5000, 5000, 3).x == 799);
    CHECK(toLogicalPoint(1200, 720, 3).x == 400);
    CHECK(toLogicalPoint(1200, 720, 3).y == 240);
    CHECK(toLogicalPoint(10, 10, 0).x == 10); // a bad scale counts as 1
}
#endif
