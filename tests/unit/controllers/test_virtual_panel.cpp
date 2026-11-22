// The virtual panel's behaviour (control-surface.md UI-140…144; virtual-panel.md UID-925…937): every
// UI-101 / UI-102 control is on it and operable with the mouse alone, hit areas are ≥ 44 × 44 lpx without overlaps,
// the gestures send what a hardware controller sends (velocity by position, latch, drag detents, rotary, knobs,
// strip), and its lamps are exactly the LampModel's.
#include "io/controllers/ControlNames.h"
#include "io/controllers/VirtualPanel.h"

#include <catch2/catch_test_macros.hpp>

#include <set>

namespace ctl = io::controllers;
using io::Control;
using Kind = io::ControlEvent::Kind;
using K = ctl::PanelItemKind;

namespace
{
std::vector<io::ControlEvent> drain(ctl::VirtualPanel& p)
{
    std::vector<io::ControlEvent> out;
    p.poll(out);
    return out;
}
ctl::VirtualPanel expanded()
{
    ctl::VirtualPanel p;
    p.setState(ctl::PanelState::Expanded);
    return p;
}
} // namespace

TEST_CASE("the virtual panel draws every UI-101 / UI-102 control, hit areas >= 44 x 44 without overlap",
          "[controllers][virtualpanel]")
{
    ctl::VirtualPanel p;
    for (const auto c : io::referenceInputControls())
    {
        if (c == Control::Screen)
            continue;
        INFO(io::nameOf(c));
        CHECK((p.item(c) != nullptr || c == Control::SelectorPush)); // sel.push = the rotary's centre
    }
    for (int c = static_cast<int>(Control::ExtCopy); c <= static_cast<int>(Control::ExtProject); ++c)
        CHECK(p.item(static_cast<Control>(c)) != nullptr);
    for (const auto c : io::lampControls())
    {
        INFO(io::lampName(c));
        CHECK(p.item(c) != nullptr); // every lamp has its place (UID-929)
    }
    const auto items = p.items();
    for (std::size_t a = 0; a < items.size(); ++a)
    {
        const auto& A = items[a];
        if (A.kind == K::Lamp || (A.kind == K::PanelButton && A.button == ctl::PanelButton::None))
            continue;
        INFO(A.label);
        CHECK(A.hit.w >= 44.0f - 0.01f);
        CHECK(A.hit.h >= 44.0f - 0.01f);
        CHECK(A.hit.x >= 0);
        CHECK(A.hit.y >= 0);
        CHECK(A.hit.x + A.hit.w <= 1280.01f);
        CHECK(A.hit.y + A.hit.h <= 480.01f);
        for (std::size_t b = a + 1; b < items.size(); ++b)
        {
            const auto& B = items[b];
            if (B.kind == K::Lamp || (B.kind == K::PanelButton && B.button == ctl::PanelButton::None))
                continue;
            const bool overlap = A.hit.x < B.hit.x + B.hit.w - 0.01f && B.hit.x < A.hit.x + A.hit.w - 0.01f
                              && A.hit.y < B.hit.y + B.hit.h - 0.01f && B.hit.y < A.hit.y + A.hit.h - 0.01f;
            INFO(B.label);
            CHECK_FALSE(overlap);
        }
    }
    // The reference positions (virtual-panel.md "Elements").
    CHECK(p.item(io::pad(13))->rect.y == 124);
    CHECK(p.item(io::pad(1))->rect.y == 292);
    CHECK(p.item(io::pad(4))->rect.x == 492);
    CHECK(p.item(io::knob(3))->rect.cx() == 120 + 2 * 208);
    CHECK(p.item(io::step(16))->rect.x == 16 + 60 * 15);
    CHECK(p.item(Control::Bar4)->rect.x == 1000 + 66 * 3);
    CHECK(p.item(Control::Pattern)->shiftLegend == "LENGTH");
    CHECK(p.item(Control::Home)->shiftLegend == "UTILITY");
}

TEST_CASE("every control is operable with the mouse alone", "[controllers][virtualpanel]")
{
    auto p = expanded();
    std::set<Control> seen;
    double t = 0;
    for (const auto& it : p.items())
    {
        if (it.kind == K::Lamp || it.kind == K::PanelButton)
            continue;
        t += 1000;
        const float x = it.hit.cx(), y = it.hit.cy();
        if (it.kind == K::Rotary)
        {
            p.pointerDown(0, x, y, {}, t); // the centre = sel.push
            p.pointerUp(0, x, y, t + 10);
            p.pointerDown(1, x + 45, y, {}, t + 20); // the rim: circular drag
            p.pointerMove(1, x, y + 45, t + 30);      // a quarter turn clockwise
            p.pointerUp(1, x, y + 45, t + 40);
        }
        else
        {
            p.pointerDown(0, x, y, {}, t);
            p.pointerMove(0, x, y - 40, t + 10);
            p.pointerUp(0, x, y - 40, t + 20);
        }
        for (const auto& e : drain(p))
            seen.insert(e.control);
    }
    for (const auto c : io::referenceInputControls())
        if (c != Control::Screen)
        {
            INFO(io::nameOf(c));
            CHECK(seen.count(c) == 1);
        }
    for (int c = static_cast<int>(Control::ExtCopy); c <= static_cast<int>(Control::ExtProject); ++c)
        CHECK(seen.count(static_cast<Control>(c)) == 1);
}

TEST_CASE("pad velocity by position, Option = 127, pressure by drag", "[controllers][virtualpanel]")
{
    auto p = expanded();
    const auto& pad = *p.item(io::pad(6));
    p.pointerDown(0, pad.rect.cx(), pad.rect.y, {}, 0); // top edge
    auto e = drain(p);
    REQUIRE(e.size() == 1);
    CHECK(e[0].kind == Kind::Press);
    CHECK(e[0].control == io::pad(6));
    CHECK(e[0].value == 127);
    CHECK(e[0].source == io::kSourceVirtual);
    p.pointerMove(0, pad.rect.cx(), pad.rect.y - 20, 10); // up = more pressure
    e = drain(p);
    REQUIRE(e.size() == 1);
    CHECK(e[0].kind == Kind::Pressure);
    CHECK(e[0].value == 40);
    CHECK(p.pressed(io::pad(6)));
    p.pointerUp(0, pad.rect.cx(), pad.rect.y - 20, 20);
    e = drain(p);
    CHECK(e.back().kind == Kind::Release);
    CHECK_FALSE(p.pressed(io::pad(6)));
    p.pointerDown(0, pad.rect.cx(), pad.rect.y + pad.rect.h - 0.01f, {}, 30); // bottom edge
    CHECK(drain(p)[0].value == 20);
    p.pointerUp(0, pad.rect.cx(), pad.rect.y + pad.rect.h - 0.01f, 40);
    drain(p);
    ctl::VirtualPanel::Mods option;
    option.option = true;
    p.pointerDown(0, pad.rect.cx(), pad.rect.y + pad.rect.h - 1, option, 50);
    CHECK(drain(p)[0].value == 127);
}

TEST_CASE("right-click, Control-click and a 500 ms hold latch, a second click lets go",
          "[controllers][virtualpanel]")
{
    auto p = expanded();
    const auto& step = *p.item(io::step(3));
    ctl::VirtualPanel::Mods right;
    right.secondary = true;
    p.pointerDown(0, step.rect.cx(), step.rect.cy(), right, 0);
    p.pointerUp(0, step.rect.cx(), step.rect.cy(), 10);
    auto e = drain(p);
    REQUIRE(e.size() == 1); // Press only: latched
    CHECK(p.latched(io::step(3)));
    // While the step is latched another pointer turns an encoder (a lock edit with one mouse).
    const auto& enc = *p.item(io::knob(2));
    p.pointerDown(1, enc.rect.cx(), enc.rect.cy(), {}, 20);
    p.pointerMove(1, enc.rect.cx(), enc.rect.cy() - 17, 30); // 17 lpx up = 2 detents
    p.pointerUp(1, enc.rect.cx(), enc.rect.cy() - 17, 40);
    e = drain(p);
    REQUIRE(e.size() == 2);
    CHECK((e[0].kind == Kind::Turn && e[0].control == io::knob(2) && e[0].value == 1));
    p.pointerDown(0, step.rect.cx(), step.rect.cy(), {}, 50); // click again: released
    p.pointerUp(0, step.rect.cx(), step.rect.cy(), 60);
    e = drain(p);
    REQUIRE(e.size() == 1);
    CHECK((e[0].kind == Kind::Release && e[0].control == io::step(3)));
    CHECK_FALSE(p.pressed(io::step(3)));

    const auto& shift = *p.item(Control::Shift);
    p.pointerDown(0, shift.rect.cx(), shift.rect.cy(), {}, 100);
    p.tick(400);
    CHECK_FALSE(p.latched(Control::Shift));
    p.tick(601); // held 500 ms without moving
    CHECK(p.latched(Control::Shift));
    p.pointerUp(0, shift.rect.cx(), shift.rect.cy(), 700);
    e = drain(p);
    REQUIRE(e.size() == 1); // the Press; no Release (latched)
    CHECK(p.shiftShown());
    ctl::VirtualPanel::Mods ctrl;
    ctrl.control = true;
    const auto& pat = *p.item(Control::Pattern);
    p.pointerDown(0, pat.rect.cx(), pat.rect.cy(), ctrl, 800);
    p.pointerUp(0, pat.rect.cx(), pat.rect.cy(), 810);
    CHECK(p.latched(Control::Pattern));
}

TEST_CASE("Shift-click is SHIFT + the key without latching SHIFT", "[controllers][virtualpanel]")
{
    auto p = expanded();
    const auto& home = *p.item(Control::Home);
    ctl::VirtualPanel::Mods shift;
    shift.shift = true;
    p.pointerDown(0, home.rect.cx(), home.rect.cy(), shift, 0);
    p.pointerUp(0, home.rect.cx(), home.rect.cy(), 10);
    const auto e = drain(p);
    REQUIRE(e.size() == 4);
    CHECK((e[0].kind == Kind::Press && e[0].control == Control::Shift));
    CHECK((e[1].kind == Kind::Press && e[1].control == Control::Home));
    CHECK((e[2].kind == Kind::Release && e[2].control == Control::Home));
    CHECK((e[3].kind == Kind::Release && e[3].control == Control::Shift));
    CHECK_FALSE(p.pressed(Control::Shift));
}

TEST_CASE("encoders 8 lpx per detent, wheel, Option = fine, rotary 15 degrees, knobs 200 lpx",
          "[controllers][virtualpanel]")
{
    auto p = expanded();
    const auto& enc = *p.item(io::knob(4));
    p.pointerDown(0, enc.rect.cx(), enc.rect.cy(), {}, 0);
    p.pointerMove(0, enc.rect.cx(), enc.rect.cy() + 24, 10); // down 24 lpx = −3
    auto e = drain(p);
    REQUIRE(e.size() == 3);
    CHECK(e[2].value == -1);
    p.pointerUp(0, enc.rect.cx(), enc.rect.cy() + 24, 20);
    ctl::VirtualPanel::Mods option;
    option.option = true;
    p.wheel(enc.rect.cx(), enc.rect.cy(), 2, option, 30);
    e = drain(p);
    REQUIRE(e.size() == 3); // fine: wrapped in SHIFT as the keyboard's Option + arrow
    CHECK((e[0].control == Control::Shift && e[0].kind == Kind::Press));
    CHECK((e[1].control == io::knob(4) && e[1].value == 2));
    CHECK(p.encoderAngle(io::knob(4)) == -1);
    CHECK(p.lastTurnMs(io::knob(4)) == 30);

    const auto& rot = *p.item(Control::Selector);
    const float cx = rot.rect.cx(), cy = rot.rect.cy();
    p.pointerDown(0, cx + 40, cy, {}, 100); // 0° (3 o'clock)
    p.pointerMove(0, cx, cy + 40, 110);     // 90° clockwise = 6 detents
    p.pointerUp(0, cx, cy + 40, 120);
    e = drain(p);
    REQUIRE(e.size() == 1);
    CHECK((e[0].control == Control::Selector && e[0].value == 6));

    const auto& knob = *p.item(Control::AfsDrive);
    CHECK(p.knobValue(Control::AfsDrive) == 512);
    p.pointerDown(0, knob.rect.cx(), knob.rect.cy(), {}, 1000);
    p.pointerMove(0, knob.rect.cx(), knob.rect.cy() - 200, 1010); // the full range
    p.pointerUp(0, knob.rect.cx(), knob.rect.cy() - 200, 1020);
    e = drain(p);
    REQUIRE_FALSE(e.empty());
    CHECK((e.back().kind == Kind::Value && e.back().control == Control::AfsDrive && e.back().value == 1023));
    p.pointerDown(0, knob.rect.cx(), knob.rect.cy(), {}, 2000);
    p.pointerUp(0, knob.rect.cx(), knob.rect.cy(), 2010);
    p.pointerDown(0, knob.rect.cx(), knob.rect.cy(), {}, 2100); // double-click = default
    p.pointerUp(0, knob.rect.cx(), knob.rect.cy(), 2110);
    e = drain(p);
    REQUIRE(e.size() == 1);
    CHECK(e[0].value == ctl::knobDefault(Control::AfsDrive));
}

TEST_CASE("the strip: press and drag inside it, bottom 0 … top 127", "[controllers][virtualpanel]")
{
    auto p = expanded();
    const auto& s = *p.item(Control::Strip);
    p.pointerDown(0, s.rect.cx(), s.rect.y + s.rect.h - 0.1f, {}, 0);
    p.pointerMove(0, s.rect.cx(), s.rect.y, 10);
    p.pointerUp(0, s.rect.cx(), s.rect.y, 20);
    const auto e = drain(p);
    REQUIRE(e.size() == 3);
    CHECK((e[0].kind == Kind::Touch && e[0].value == 0));
    CHECK((e[1].kind == Kind::Value && e[1].value == 127));
    CHECK((e[2].kind == Kind::Release && e[2].control == Control::Strip));
    CHECK(p.stripPosition() == -1);
}

TEST_CASE("panel lamps are the LampModel's, other sources show as pressed, drawer states",
          "[controllers][virtualpanel]")
{
    ctl::VirtualPanel p;
    p.setLamp(io::pad(3), 0x123456, 200);
    p.setLamp(io::barPositionLamp(2), 0xFFFFFF, 255);
    CHECK(p.lamp(io::pad(3)).rgb == 0x123456);
    CHECK(p.lamp(io::pad(3)).level == 200);
    CHECK(p.lamp(io::barPositionLamp(2)).level == 255);
    p.observe({Kind::Press, Control::Rec, 0, io::kSourceKeyboard, 0});
    CHECK(p.pressed(Control::Rec));
    p.observe({Kind::Release, Control::Rec, 0, io::kSourceKeyboard, 0});
    CHECK_FALSE(p.pressed(Control::Rec));
    // Hidden: no input; the drawer bar toggles collapsed / expanded; HIDE hides; KEYS toggles the keycaps.
    CHECK_FALSE(p.pointerDown(0, 100, 300, {}, 0));
    p.toggle();
    CHECK(p.state() == ctl::PanelState::Expanded);
    CHECK(p.height() == 480);
    p.pointerDown(0, 300, 16, {}, 0);
    p.pointerUp(0, 300, 16, 0);
    CHECK(p.state() == ctl::PanelState::Collapsed);
    CHECK(p.height() == 32);
    CHECK_FALSE(p.pointerDown(0, 300, 200, {}, 0)); // collapsed: only the bar
    p.pointerDown(0, 1080, 16, {}, 0);
    p.pointerUp(0, 1080, 16, 0);
    CHECK(p.keycaps());
    p.pointerDown(0, 1224, 16, {}, 0);
    p.pointerUp(0, 1224, 16, 0);
    CHECK(p.state() == ctl::PanelState::Hidden);
    CHECK(drain(p).empty()); // panel UI sends no control events
}
