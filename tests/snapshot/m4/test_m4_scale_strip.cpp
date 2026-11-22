// SET SCALE (+ MIDI) and TOUCH STRIP SETTING (playback.md "SET SCALE", utility.md "TOUCH STRIP SETTING"):
// SET SCALE against set-scale.svg from its new home, the MIDI variant's note names and columns, the SCALE popup's icon
// column; TOUCH STRIP SETTING against touch-strip-setting.svg, the eight places, SELECT PARAMETER from stripTargets(),
// BOTTOM / TOP in the target's format, DELETE (disabled on an empty place, confirmed), the StripPreview; the registry.
#include "core/commands/ribbon/RibbonCommands.h"
#include "tests/snapshot/m4/M4Fixture.h"
#include "ui/screens/lfo/Targets.h"
#include "ui/screens/touchstrip/StripLive.h"
#include "ui/shell/Layout.h"
#include "ui/widgets/ListPopup.h"
#include "ui/widgets/ScaleMap.h"
#include "ui/widgets/StripPreview.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace snapshot;
using m4test::M4Fixture;
namespace L = ui::layout;
namespace rb = core::commands::ribbon;

TEST_CASE("snapshot.m4.set_scale: SET SCALE (ui/screens/setscale) matches set-scale.svg", "[snapshot][m4]")
{
    Session session;
    M4Fixture m;
    m.shell().selectTrack(5);
    m.set("scale.type", 5, 2); // MINOR
    REQUIRE(m.open("track.playback"));
    REQUIRE(m.open("track.playback.setScale"));
    CHECK(m.shell().railFocus() == 0); // SCALE holds the focus
    const auto ours = m.f.image();
    const auto& mock = m4test::m4mockup("set-scale");
    CHECK(compare(ours, mock, ui::ScaleMap::kPanel, "m4-set-scale-degrees").match >= kGate);
    CHECK(compare(ours, mock, ui::PadMap::kPanel, "m4-set-scale-pads").match >= kGate);
    CHECK(compare(ours, mock, L::canvas, "m4-set-scale").match >= kGate);
}

TEST_CASE("m4ui.set_scale: the MIDI variant shows note names from ROOT; AREA / ROOT columns; SCALE list", "[m4]")
{
    M4Fixture m;
    m.shell().selectTrack(10);
    REQUIRE(m.open("track.setScaleMidi"));
    CHECK(m.shell().current().parent == "track.menu");
    CHECK(m.shell().boundColumn(0)->id == "scaleMidi.area");
    CHECK(m.shell().boundColumn(1)->id == "scaleMidi.root");
    CHECK(m.shell().boundColumn(2) == nullptr);
    CHECK(m.hasText("C5")); // pad 1 = the root C5 (60), chromatic
    CHECK(m.hasText("D#5"));
    m.f.turn(io::knob(2), 2); // ROOT D5
    m.f.settle();
    CHECK(m.hasText("D5"));
    CHECK_FALSE(m.hasText("C5"));
    m.f.press(io::Control::SelectorPush); // SCALE popup, CHROMATIC current
    REQUIRE(m.shell().popupDepth() == 1);
    m.f.turn(io::Control::Selector, 1);
    m.f.press(io::Control::SelectorPush);
    m.f.settle();
    CHECK(m.get("scaleMidi.type", 10) == 1);
    CHECK(m.get("scale.type", 10) == 0); // the sample-track scale is a different parameter
    // The SCALE popup draws a degree strip per row (12 ticks: 7 filled for MAJOR on the current row).
    m.f.press(io::Control::SelectorPush);
    m.f.settle();
    const auto& dark = ui::theme::palette(true);
    int ticks = 0;
    for (const auto& c : m.f.dl.commands())
        ticks += c.op == ui::DrawCmd::Op::Fill && c.argb == dark.accent && c.rect.w == 1.5f;
    CHECK(ticks == 7);
}

namespace
{
void assignDemo(M4Fixture& m)
{
    auto& s = ui::m2::Session::instance();
    s.execute(rb::assignItem(s.project(), 0, 0, "playback.pitch"));
    s.execute(rb::setItemRange(s.project(), 0, 0, 0.5, 0.75)); // 0.0 st … +12.0 st
    s.execute(rb::assignItem(s.project(), 0, 1, "fx1.filter.cutoff"));
    s.execute(rb::setItemRange(s.project(), 0, 1, 1.0, 0.318)); // 20.0 kHz … 180 Hz
    s.execute(rb::assignItem(s.project(), 0, 2, "track.volume"));
    REQUIRE(rb::itemCount(s.project().ribbon, 0) == 3);
    (void)m;
}
} // namespace

TEST_CASE("snapshot.m4.touch_strip: TOUCH STRIP SETTING (USER1) matches touch-strip-setting.svg", "[snapshot][m4]")
{
    Session session;
    M4Fixture m;
    assignDemo(m);
    REQUIRE(m.open("utility"));
    REQUIRE(m.open("utility.touchStrip.user1"));
    m.f.turn(io::Control::Selector, 1); // place 2
    auto& live = ui::m4::stripLive();
    live = {2, true, 40}; // USER1, touched
    const auto ours = m.f.image();
    live = {};
    const auto& mock = m4test::m4mockup("touch-strip-setting");
    CHECK(compare(ours, mock, L::canvas, "m4-touch-strip").match >= kGate);
    CHECK(compare(ours, mock, {224, 152, 808, 168}, "m4-touch-strip-head").match >= kGate); // title, header, 1–2
    CHECK(compare(ours, mock, ui::StripPreview::kPanel, "m4-touch-strip-preview").match >= kGate);
    CHECK(compare(ours, mock, L::knobStrip, "m4-touch-strip-strip").match >= kGate);
    CHECK(m.shell().stripTab() == "USER1 \xC2\xB7 2");
    CHECK(m.hasText("3 of 8 assigned"));
    CHECK(m.hasText("PLAYBACK \xE2\x80\xBA PITCH"));
    CHECK(m.hasText("FX1 \xE2\x80\xBA FILTER CUT OFF"));
    CHECK(m.hasText("\xE2\x80\x94 empty \xC2\xB7 push to assign"));
}

TEST_CASE("m4ui.touch_strip: places, SELECT PARAMETER, BOTTOM / TOP, DELETE — by panel only", "[m4]")
{
    M4Fixture m;
    assignDemo(m);
    REQUIRE(m.open("utility"));
    // UTILITY (interim list): the rotary reaches the USER1 / USER2 rows, push opens the page, BACK returns.
    m.f.press(io::Control::SelectorPush);
    m.f.settle();
    REQUIRE(m.shell().current().id == "utility.touchStrip.user1");
    auto& s = ui::m2::Session::instance();
    const auto& reg = core::builtinParams();
    // Columns: place 1 = PITCH 0.0 st / +12.0 st in the target's format; a turn moves BOTTOM in PITCH steps.
    INFO(std::string(m.shell().column(0).valueText) + " / " + std::string(m.shell().column(1).valueText));
    CHECK(m.shell().column(0).valueText == "0.0 st");
    CHECK(m.shell().column(1).valueText == "+12.0 st");
    m.f.turn(io::knob(1), -2);
    m.f.settle();
    const auto* pitch = reg.find("playback.pitch");
    CHECK(std::abs(s.project().ribbon.user[0][0].bottom -
                   (0.5 - 2.0 * static_cast<double>(pitch->coarseStep / (pitch->maxValue - pitch->minValue)))) < 1e-6);
    // The rotary walks the eight places; place 4 is empty: columns disabled, DELETE disabled.
    m.f.turn(io::Control::Selector, 3);
    m.f.settle();
    CHECK(ui::touchStripPlace(m.shell().body()) == 3);
    CHECK(m.shell().stripTab() == "USER1 \xC2\xB7 4");
    CHECK(m.shell().column(0).disabled);
    // Push on the empty place: SELECT PARAMETER, generated from stripTargets(), NONE first.
    m.f.press(io::Control::SelectorPush);
    auto* list = dynamic_cast<ui::ListPopup*>(m.shell().topPopup());
    REQUIRE(list != nullptr);
    std::vector<ui::ListItem> items;
    ui::m4::buildTargets(reg, ui::m4::TargetKind::Strip, 0, 0, items);
    CHECK(items.front().label == "NONE");
    for (std::size_t i = 1; i < items.size(); ++i)
        CHECK(reg[core::ParamId{static_cast<std::uint16_t>(items[i].value)}].stripTarget);
    CHECK(items[static_cast<std::size_t>(list->focusedItem())].group == "PLAYBACK"); // an empty place opens on PLAYBACK
    m.f.turn(io::Control::Selector, 1);
    m.f.press(io::Control::SelectorPush);
    m.f.settle();
    REQUIRE(rb::itemCount(s.project().ribbon, 0) == 4);
    CHECK(s.project().ribbon.user[0][3].target == reg[core::ParamId{static_cast<std::uint16_t>(items[2].value)}].id);
    // DELETE: the rotary past place 1 reaches it; push asks; DELETE removes, the later items move up.
    m.f.turn(io::Control::Selector, -10);
    m.f.turn(io::Control::Selector, -1);
    CHECK(m.shell().railFocus() == 0);
    m.f.press(io::Control::SelectorPush);
    REQUIRE(m.shell().popupDepth() == 1);
    m.f.turn(io::Control::Selector, 1); // CANCEL → DELETE
    m.f.press(io::Control::SelectorPush);
    m.f.settle();
    CHECK(rb::itemCount(s.project().ribbon, 0) == 3);
    CHECK(s.project().ribbon.user[0][0].target == "fx1.filter.cutoff");
    // An undo restores it (every edit one undo step).
    CHECK(s.undo());
    CHECK(rb::itemCount(s.project().ribbon, 0) == 4);
    m.f.press(io::Control::Back);
    m.f.settle();
    CHECK(m.shell().current().id == "utility");
}

TEST_CASE("m4ui.touch_strip: the StripPreview follows the finger in any strip mode", "[m4]")
{
    M4Fixture m;
    assignDemo(m);
    REQUIRE(m.open("utility.touchStrip.user1"));
    auto& live = ui::m4::stripLive();
    const auto& dark = ui::theme::palette(true);
    auto playheadLine = [&] {
        for (const auto& c : m.f.dl.commands())
            if (c.op == ui::DrawCmd::Op::Fill && c.argb == dark.playhead && c.rect.x > 1048)
                return true;
        return false;
    };
    live = {0, true, 64}; // PITCH mode touched: the preview shows the finger too (hardware feedback)
    m.f.settle();
    CHECK(playheadLine());
    live = {2, true, 64};
    m.f.settle();
    CHECK(playheadLine());
    CHECK(m.shell().body()->dirty(m.f.now));
    live = {};
    m.f.settle();
    CHECK_FALSE(playheadLine());
}

TEST_CASE("m4ui.registry: the performance screens validate, with the spec's parents and strips", "[m4]")
{
    const auto& screens = ui::ScreenRegistry::builtin();
    const auto problems = screens.validate(ui::WidgetRegistry::builtin().dispatcher(), core::builtinParams());
    INFO((problems.empty() ? std::string() : problems.front()));
    CHECK(problems.empty());
    struct Want
    {
        std::string_view id, parent, title;
    };
    constexpr Want want[] = {{"track.lfo", "track.menu", "LFO"},
                             {"track.playback.setScale", "track.playback", "SET SCALE"},
                             {"track.setScaleMidi", "track.menu", "SET SCALE"},
                             {"utility.touchStrip.user1", "utility", "TOUCH STRIP SETTING (USER1)"},
                             {"utility.touchStrip.user2", "utility", "TOUCH STRIP SETTING (USER2)"}};
    for (const auto& w : want)
    {
        const auto* s = screens.find(w.id);
        REQUIRE(s != nullptr);
        CHECK(s->parent == w.parent);
        CHECK(s->title == w.title);
    }
    const auto* lfo = screens.find("track.lfo");
    CHECK(lfo->strip.columns[0] == "lfo.speed");
    CHECK(lfo->strip.columns[1] == "lfo.depth");
    CHECK(lfo->strip.columns[2] == "lfo.phase"); // UID-732: columns 1–3 never move
    CHECK(lfo->strip.columns[3] == "lfo.destination");
    CHECK(lfo->strip.columns[4] == "lfo.shape");
    CHECK(lfo->rail.size() == 4);
    CHECK(lfo->rail[0].label == "DESTINATION");
    CHECK(lfo->rail[3].binds == "lfo.sync");
}
