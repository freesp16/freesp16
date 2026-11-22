// LFO (amp-envelope-lfo.md "LFO", UID-732…739): the screen against lfo.svg / lfo-destination.svg /
// lfo-setting.svg (track 5, TRIANGLE to PITCH, SYNC 1/2, DEPTH 63, playing), the generated DESTINATION list, the stale
// FX destination, keyboard / panel reachability, the live dot, the lock overlay and the graph cache.
#include "core/util/lfo/LfoLaw.h"
#include "tests/snapshot/m4/M4Fixture.h"
#include "ui/screens/lfo/Targets.h"
#include "ui/shell/Layout.h"
#include "ui/widgets/ListPopup.h"
#include "ui/widgets/LfoGraph.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>

using namespace snapshot;
using m4test::M4Fixture;
namespace L = ui::layout;

namespace
{
constexpr int kTrack = 5;

// The mockups draw the triangle from its peak (kit_track.py: 4·|f − ½| − 1); the engine's TRIANGLE (LFO-001,
// engine/modulation/Lfo.cpp) runs 0 → +1 → −1 → 0 in phase with the sine, and the graph draws the engine's law. The
// mockup's curve is the engine's at PHASE OFFSET 90°, so the snapshots set 32 (= 90°); column 3 then reads 90°.
constexpr float kMockupPhase = 32;

void mockupState(M4Fixture& m, float phase = kMockupPhase)
{
    m.shell().selectTrack(kTrack);
    m.set("lfo.shape", kTrack, 1); // TRIANGLE
    m.set("lfo.sync", kTrack, 1);
    m.set("lfo.speed", kTrack, core::lfo::syncDivisionKnob(5)); // 1/2
    m.set("lfo.depth", kTrack, 63);
    m.set("lfo.phase", kTrack, phase);
    m.set("lfo.destination", kTrack, m.paramId("playback.pitch"));
    REQUIRE(m.open("track.menu"));
    REQUIRE(m.open("track.lfo"));
}

// The mockup's live dot sits at 62 % of the 2-bar window: start at PPQ 0, then 4.96 quarters later.
void playing(M4Fixture& m)
{
    m.play(0);
    m.f.frame();
    m.play(0.62 * 8);
    m.f.frame();
}
} // namespace

TEST_CASE("snapshot.m4.lfo: LFO matches lfo.svg (track 5, TRIANGLE to PITCH, SYNC 1/2, playing)", "[snapshot][m4]")
{
    Session session;
    M4Fixture m;
    mockupState(m);
    playing(m);
    const auto ours = render(m.f.frame(), 1280, 768);
    const auto& mock = m4test::m4mockup("lfo");
    CHECK(compare(ours, mock, L::canvas, "m4-lfo").match >= kGate);
    CHECK(compare(ours, mock, ui::LfoGraph::kPanel, "m4-lfo-graph").match >= kGate);
    CHECK(compare(ours, mock, {16, 152, 192, 456}, "m4-lfo-rail").match >= kGate);
    CHECK(compare(ours, mock, L::knobStrip, "m4-lfo-strip").match >= kGate);
    CHECK(m.hasText("1/2"));   // SPEED as the SYNC division
    CHECK(m.hasText("PITCH")); // DESTINATION (rail and column 4)
    CHECK(m.hasText("BAR 2"));
    const auto* g = ui::lfoGraphOf(m.shell().body());
    REQUIRE(g != nullptr);
    CHECK(g->liveOn());
}

TEST_CASE("snapshot.m4.lfo_destination: the DESTINATION popup's head matches lfo-destination.svg", "[snapshot][m4]")
{
    Session session;
    M4Fixture m;
    mockupState(m);
    playing(m);
    m.f.press(io::Control::SelectorPush); // the focus starts on DESTINATION
    REQUIRE(m.shell().popupDepth() == 1);
    const auto ours = m.f.image(); // after the open transition
    const auto& mock = m4test::m4mockup("lfo-destination");
    // Title band, NONE, the PLAYBACK header and PITCH (current, ✓). The rows after PITCH differ (our choice):
    // START / LOOP START / LENGTH are no LFO targets (the engine does not modulate them), so AMP ENVELOPE follows.
    CHECK(compare(ours, mock, {320, 184, 640, 48 + 3 * 44}, "m4-lfo-destination-head").match >= kGate);
    CHECK(compare(ours, mock, L::knobStrip, "m4-lfo-destination-strip").match >= kGate);
}

TEST_CASE("snapshot.m4.lfo_setting: SETTING matches lfo-setting.svg (Mode, Trigger On, Random Start)", "[snapshot][m4]")
{
    Session session;
    M4Fixture m;
    mockupState(m);
    m.set("lfo.triggerMode", kTrack, 1);
    m.set("lfo.triggerOn", kTrack, 0);
    playing(m);
    m.f.turn(io::Control::Selector, 2); // DESTINATION → SHAPE → SETTING
    m.f.press(io::Control::SelectorPush);
    REQUIRE(m.shell().popupDepth() == 1);
    const auto ours = m.f.image(); // after the open transition
    const auto& mock = m4test::m4mockup("lfo-setting");
    // The shell centres popups on the 4 lpx grid (layout::centred: floor4 → y 292), the spec places this one at y 294
    // (also behind m1-amp-envelope-setting's 97.0 %): the popup is compared where each one draws it.
    CHECK(compare(ours, {320, 292, 640, 180}, mock, {320, 294, 640, 180}, "m4-lfo-setting-popup").match >= kGate);
    CHECK(compare(ours, mock, L::canvas, "m4-lfo-setting").match >= kGate);
    CHECK(m.hasText("Random Start"));
}

TEST_CASE("m4ui.lfo: the DESTINATION list is generated from lfoTargets(), NONE first, FX of the slot's effect", "[m4]")
{
    M4Fixture m;
    m.slots.selectEffect(ui::fx::Slot::Fx1, 0, kTrack - 1, "filter");
    mockupState(m);
    m.f.press(io::Control::SelectorPush);
    auto* popup = dynamic_cast<ui::ListPopup*>(m.shell().topPopup());
    REQUIRE(popup != nullptr);
    std::vector<ui::ListItem> items;
    ui::m4::buildTargets(core::builtinParams(), ui::m4::TargetKind::Lfo, 0, kTrack - 1, items);
    REQUIRE(! items.empty());
    CHECK(items.front().label == "NONE");
    CHECK(items.front().value == -1);
    // Every non-FX LFO target exactly once, FX1 only the filter's (not its .div twins), FX2 none (empty slot).
    std::set<int> listed;
    for (std::size_t i = 1; i < items.size(); ++i)
        CHECK(listed.insert(items[i].value).second);
    const auto& reg = core::builtinParams();
    int expected = 0;
    for (const auto id : reg.lfoTargets())
    {
        const auto sid = reg[id].id;
        int slot = -1;
        const auto effect = ui::m4::fxEffectOf(sid, slot);
        const bool want = reg[id].featureFlag.empty() &&
                          (slot < 0 || (slot == 0 && effect == "filter" && sid.substr(sid.size() - 4) != ".div"));
        CHECK(listed.count(static_cast<int>(id.value)) == (want ? 1u : 0u));
        expected += want ? 1 : 0;
    }
    CHECK(static_cast<int>(listed.size()) == expected);
    CHECK(listed.count(static_cast<int>(reg.idOf("env.attack")->value)) == 1);
    // Groups in module order with the effect's name; the current destination (PITCH) is focused.
    CHECK(items[1].group == "PLAYBACK");
    bool fx1 = false;
    for (const auto& it : items)
        fx1 |= it.group == "FX1 : MULTI MODE FILTER";
    CHECK(fx1);
    CHECK(popup->focusedItem() == 1);
    // Choosing writes lfo.destination; NONE clears it.
    m.f.turn(io::Control::Selector, 1);
    m.f.press(io::Control::SelectorPush);
    m.f.settle();
    CHECK(m.get("lfo.destination", kTrack) == static_cast<float>(items[2].value));
}

TEST_CASE("m4ui.lfo: a destination whose effect left its slot shows text-3 with a warning", "[m4]")
{
    M4Fixture m;
    m.slots.selectEffect(ui::fx::Slot::Fx1, 0, kTrack - 1, "filter");
    mockupState(m);
    m.set("lfo.destination", kTrack, m.paramId("fx1.filter.cutoff"));
    m.f.settle();
    const auto name = ui::m4::targetName(core::builtinParams(), *core::builtinParams().idOf("fx1.filter.cutoff"));
    CHECK(name.substr(0, 4) == "FX1 ");
    const auto& dark = ui::theme::palette(true);
    CHECK(m.textColour(name) == dark.text1); // in its slot: normal
    CHECK_FALSE(m.shell().column(3).inactive);
    m.slots.selectEffect(ui::fx::Slot::Fx1, 0, kTrack - 1, "delay"); // LFO-013: the effect left the slot
    m.f.settle();
    CHECK(ui::m4::targetStale(core::builtinParams(), *core::builtinParams().idOf("fx1.filter.cutoff"), 0, kTrack - 1));
    CHECK(m.textColour(name) == dark.text3);
    CHECK(m.shell().column(3).inactive);
    bool warn = false; // the ⚠ glyph is drawn in `warn` beside the value box
    for (const auto& c : m.f.dl.commands())
        warn |= c.op == ui::DrawCmd::Op::Fill && c.argb == dark.warn && c.rect.x < 208 && c.rect.y > 152 && c.rect.y < 254;
    CHECK(warn);
}

TEST_CASE("m4ui.lfo: rail, popups and columns are reachable by panel; SYNC steps SPEED by division", "[m4]")
{
    M4Fixture m;
    mockupState(m);
    CHECK(m.shell().railFocus() == 0);
    m.f.turn(io::Control::Selector, 1); // SHAPE
    m.f.press(io::Control::SelectorPush);
    REQUIRE(m.shell().popupDepth() == 1);
    m.f.turn(io::Control::Selector, 2); // TRIANGLE → SQUARE
    m.f.press(io::Control::SelectorPush);
    m.f.settle();
    CHECK(m.get("lfo.shape", kTrack) == 3);
    CHECK(m.hasText("SQUARE"));
    m.f.turn(io::Control::Selector, 2); // SYNC
    m.f.press(io::Control::SelectorPush);
    m.f.settle();
    CHECK(m.get("lfo.sync", kTrack) == 0);
    CHECK(m.hasText("1 s")); // free window labels
    m.f.press(io::Control::SelectorPush);
    m.f.settle();
    CHECK(m.get("lfo.sync", kTrack) == 1);
    // Column 1 in SYNC: one detent = one division (1/2 → 1/4.).
    m.f.turn(io::knob(1), 1);
    m.f.settle();
    CHECK(core::lfo::syncDivisionIndex(m.get("lfo.speed", kTrack)) == 6);
    CHECK(m.shell().column(0).valueText == "1/4.");
    // Columns 2–3 as the shell steps them; 4 walks the generated list; 5 the shape.
    m.f.turn(io::knob(2), -3);
    m.f.turn(io::knob(3), -32);
    m.f.turn(io::knob(4), 1);
    m.f.turn(io::knob(5), -1);
    m.f.settle();
    CHECK(m.get("lfo.depth", kTrack) == 60);
    CHECK(m.get("lfo.phase", kTrack) == 0);
    std::vector<ui::ListItem> items;
    ui::m4::buildTargets(core::builtinParams(), ui::m4::TargetKind::Lfo, 0, kTrack - 1, items);
    CHECK(m.get("lfo.destination", kTrack) == static_cast<float>(items[2].value)); // PITCH → the next target
    CHECK(m.get("lfo.shape", kTrack) == 2);
    CHECK(m.shell().column(3).stepped);
    CHECK(m.shell().column(3).valueText == ui::m4::targetName(core::builtinParams(),
                                                               core::ParamId{static_cast<std::uint16_t>(items[2].value)}));
    // SETTING: Mode, Trigger On, Random Start (push cycles the focused row's value).
    m.f.turn(io::Control::Selector, -1); // SYNC → SETTING
    m.f.press(io::Control::SelectorPush);
    REQUIRE(m.shell().popupDepth() == 1);
    m.f.press(io::Control::SelectorPush); // Mode: the value side
    m.f.turn(io::Control::Selector, 1);  // TRIGGER → ONE SHOT
    m.f.press(io::Control::SelectorPush); // confirm
    m.f.press(io::Control::Back);
    m.f.settle();
    CHECK(m.get("lfo.triggerMode", kTrack) == 2);
}

TEST_CASE("m4ui.lfo: the graph caches its waveform; the live dot runs only with the transport; locks overlay", "[m4]")
{
    M4Fixture m;
    mockupState(m);
    const auto* g = ui::lfoGraphOf(m.shell().body());
    REQUIRE(g != nullptr);
    const int before = g->recomputes();
    for (int i = 0; i < 30; ++i)
        m.f.frame();
    CHECK(g->recomputes() == before); // no parameter change: no recompute
    CHECK_FALSE(g->liveOn());
    CHECK_FALSE(m.shell().body()->dirty(m.f.now));
    m.play(0);
    m.f.frame();
    CHECK(g->liveOn());
    CHECK(m.shell().body()->dirty(m.f.now));
    m.play(2.0); // one LFO cycle (1/2 = 2 quarters) later: back at the start value
    m.f.frame();
    CHECK(g->liveOn());
    m.play(2.0, false);
    m.f.frame();
    CHECK_FALSE(g->liveOn());
    // DEPTH changes the cache once; amplitude follows DEPTH / 127.
    m.set("lfo.depth", kTrack, 127);
    m.f.frame();
    CHECK(g->recomputes() == before + 1);
    float top = 1e9f;
    for (int i = 0; i <= ui::LfoGraph::kColumns; ++i)
        top = std::min(top, g->yAt(i));
    CHECK(top == ui::LfoGraph::kPlot.y); // full depth reaches the plot's top
    // A held step's locks: the dashed mod overlay.
    const auto* spec = core::builtinParams().find("lfo.depth");
    m.shell().values().setLock(*spec, m.state().trackScope(), 3, 20);
    m.state().stepHeld = 3;
    m.f.frame();
    const auto& dark = ui::theme::palette(true);
    int modCols = 0;
    for (const auto& c : m.f.dl.commands())
        modCols += c.op == ui::DrawCmd::Op::Fill && c.argb == dark.mod && c.rect.w == 2 &&
                   c.rect.x >= ui::LfoGraph::kPlot.x && c.rect.y > ui::LfoGraph::kPlot.y;
    CHECK(modCols > 100);
    m.state().stepHeld = 0;
}

TEST_CASE("m4ui.lfo: NONE dims the waveform and says so; BYPASS shows the badge", "[m4]")
{
    M4Fixture m;
    mockupState(m);
    m.set("lfo.destination", kTrack, -1);
    m.f.settle();
    CHECK(m.hasText("NO DESTINATION"));
    CHECK(m.hasText("NONE"));
    m.set("lfo.bypass", kTrack, 1);
    m.f.settle();
    CHECK(m.hasText("BYPASS"));
    // BACK returns to TRACK MENU (the navigation graph).
    m.f.press(io::Control::Back);
    m.f.settle();
    CHECK(m.shell().current().id == "track.menu");
}
