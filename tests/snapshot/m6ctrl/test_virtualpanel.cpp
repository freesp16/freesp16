// The virtual panel against its mockups (07-ui-design/screens/virtual-panel.md, mockups
// virtual-panel.svg "drawer, playing" and virtual-panel-keys.svg "keyboard map"): the panel in the mockup's state
// (lamps set as the LampModel would, the strip touched, encoder 3 just turned, the knobs at the drawn values) is
// rendered with the soft backend and compared region by region under the harness's tolerance (SnapshotTest.h). The
// copies of the SVGs are in tests/snapshot/m6ctrl/mockups (SOURCE.txt). Every region is gated at kGate; what the
// DrawList cannot draw (arcs, lines) is drawn as dots, the encoders' knurl ticks are left out (they blur away).
#include "tests/snapshot/SnapshotTest.h"

#include "io/controllers/VirtualPanel.h"
#include "io/controllers/profiles/KeyRemap.h"
#include "ui/screens/virtualpanel/PanelView.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <string>
#include <vector>

namespace
{
namespace ctl = io::controllers;
using io::Control;
using snapshot::compare;
using snapshot::kGate;

const snapshot::Image& panelMockup(std::string_view name)
{
    static std::vector<std::string> keys;
    keys.reserve(8);
    keys.push_back("../../../tests/snapshot/m6ctrl/mockups/" + std::string(name));
    return snapshot::mockup(keys.back());
}

std::uint32_t mixWhite(std::uint32_t rgb, float f)
{
    auto ch = [&](int s) {
        const float c = static_cast<float>(rgb >> s & 255u);
        return static_cast<std::uint32_t>(c + (255.0f - c) * f + 0.5f) << s;
    };
    return ch(16) | ch(8) | ch(0);
}

// The "playing" state of virtual-panel.svg.
void playing(ctl::VirtualPanel& p, double now)
{
    p.setState(ctl::PanelState::Expanded);
    const std::pair<int, std::uint32_t> pads[] = {{1, 0xFFFFFF}, {2, 0xFD8358}, {3, 0xF68C36}, {5, mixWhite(0xD9A100, 0.35f)},
                                                  {6, 0xC3AC00}, {10, 0x00C7D2}, {11, 0x3FB3FF}};
    for (const auto& [n, c] : pads)
        p.setLamp(io::pad(n), c, 255);
    for (int n : {1, 5, 9, 13})
        p.setLamp(io::step(n), 0xFF7E72, 255);
    p.setLamp(io::step(11), 0xFFFFFF, 255);
    p.setLamp(Control::Bar1, 0x569DFF, 255);
    p.setLamp(io::barPositionLamp(1), 0xE8ECF2, 255);
    for (int k = 0; k < 3; ++k)
        p.setLamp(static_cast<Control>(static_cast<int>(Control::StripLed1) + k), 0xE8ECF2, 255);
    p.setLamp(Control::StripModePitch, 0xE8ECF2, 255);
    p.setLamp(Control::Play, 0x4ADD8C, 255);
    p.setLamp(Control::AfsActive, 0xF53C41, 255);
    const std::pair<Control, int> knobs[] = {{Control::MainVolume, 767}, {Control::AfsDrive, 307},
                                             {Control::AfsCutoff, 870}, {Control::AfsResonance, 205},
                                             {Control::AfsHpCutoff, 0}};
    for (const auto& [c, v] : knobs)
        p.observe({io::ControlEvent::Kind::Value, c, static_cast<std::int16_t>(v), io::kSourcePanel, 0});
    const int turns[] = {0, 3, 5, 8, 11, 13}; // the mockup's dots sit 40° apart; 15° per detent comes close
    for (int n = 1; n <= 6; ++n)
        p.observe({io::ControlEvent::Kind::Turn, io::knob(n), static_cast<std::int16_t>(turns[n - 1]), io::kSourcePanel,
                   n == 3 ? now - 100 : -10000});
    const auto& strip = *p.item(Control::Strip);
    p.pointerDown(0, strip.rect.cx(), 303.3f, {}, now);
    std::vector<io::ControlEvent> drop;
    p.poll(drop);
}

struct Region
{
    const char* name;
    ui::RectF rect;
    double gate;
};
} // namespace

TEST_CASE("virtualpanel: the drawer while playing matches virtual-panel.svg", "[snapshot][virtualpanel]")
{
    snapshot::Session session;
    ctl::VirtualPanel panel;
    const double now = 10000;
    playing(panel, now);
    ui::DrawList dl;
    dl.reserve(4096, 16384);
    ui::virtualpanel::drawPanel(dl, panel, ui::theme::palette(), 0, 0, now);
    const auto ours = snapshot::render(dl);
    const auto& mock = panelMockup("virtual-panel");
    REQUIRE(mock.w == 1280);
    REQUIRE(mock.h == 480);
    const Region regions[] = {
        {"vp-drawer-bar", {0, 0, 1280, 33}, kGate},
        {"vp-pads", {184, 118, 408, 228}, kGate},
        {"vp-function-row", {184, 346, 944, 52}, kGate},
        {"vp-sixteen-keys", {600, 200, 548, 150}, kGate},
        {"vp-steps-bars", {8, 400, 1264, 76}, kGate},
        {"vp-strip", {8, 118, 176, 280}, kGate},
        {"vp-active", {1128, 126, 96, 52}, kGate},
        {"vp-encoders", {0, 36, 1280, 76}, kGate}, // knurl ticks (24 lines per encoder) are not drawn: blurred away
        {"vp-knobs", {600, 118, 528, 80}, kGate},    // 270° arcs and pointer lines drawn as dots here
        {"vp-rotary", {1148, 240, 112, 112}, kGate},
        {"vp-whole", {0, 0, 1280, 480}, kGate},
    };
    for (const auto& r : regions)
    {
        INFO(r.name);
        const auto res = compare(ours, mock, r.rect, r.name);
        CHECK(res.match >= r.gate);
    }
}

TEST_CASE("virtualpanel: KEYS on shows every control's key as a keycap like virtual-panel-keys.svg",
          "[snapshot][virtualpanel]")
{
    snapshot::Session session;
    ctl::keyRemap().resetAll();
    ctl::VirtualPanel panel;
    playing(panel, 10000);
    panel.setKeycaps(true);
    ui::DrawList dl;
    dl.reserve(4096, 16384);
    ui::virtualpanel::drawPanel(dl, panel, ui::theme::palette(), 0, 0, 20000);
    const auto ours = snapshot::render(dl);
    const auto& mock = panelMockup("virtual-panel-keys");
    REQUIRE(mock.w == 1280);
    const Region regions[] = {
        {"vpk-sixteen-keys", {600, 200, 548, 150}, kGate},
        {"vpk-function-row", {184, 346, 944, 52}, kGate},
        {"vpk-steps", {8, 400, 976, 76}, kGate},
        {"vpk-whole", {0, 0, 1280, 480}, kGate},
    };
    for (const auto& r : regions)
    {
        INFO(r.name);
        CHECK(compare(ours, mock, r.rect, r.name).match >= r.gate);
    }
    // Every control with a key shows one (pads, steps, transport, ext keys) — the text is in the DrawList.
    int caps = 0;
    for (const auto& c : dl.commands())
        if (c.op == ui::DrawCmd::Op::Text && dl.textOf(c) == "Z")
            ++caps;
    CHECK(caps == 1);
}
