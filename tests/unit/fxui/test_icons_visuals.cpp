// Effect icons (components/visuals.md "Icons", UID-400) and the seven visual kinds (UID-834/835), plus the frame's
// allocation rule (no allocation after warm-up with the panel open, meters animating and knobs turning).
#include "core/util/Realtime.h"
#include "tests/unit/fxui/FxuiTest.h"
#include "ui/widgets/EffectIcon.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

using namespace fxuitest;
namespace fx = ui::fx;

TEST_CASE("fxui.icons_svg: resources/icons/fx/<id>.svg are exactly the drawn icon family", "[fxui]")
{
    const bool write = std::getenv("FXUI_WRITE_ICONS") != nullptr; // regenerate after editing an icon
    for (const auto& icon : ui::effectIcons())
    {
        const std::string path = std::string(APP_SOURCE_DIR) + "/resources/icons/fx/" + std::string(icon.id) + ".svg";
        const auto svg = ui::effectIconSvg(icon);
        if (write)
            std::ofstream(path) << svg;
        std::ifstream in(path);
        std::stringstream file;
        file << in.rdbuf();
        INFO(path);
        CHECK(file.str() == svg);
    }
}

TEST_CASE("fxui.icons_family: NONE, the twelve reference effects and EXT, cached at 24 / 40 / 48 lpx", "[fxui]")
{
    for (const auto id : {"none", "chorus", "flanger", "phaser", "eq2", "filter", "delay", "delayBus", "reverb", "lofi",
                          "distortion", "compressor", "ducker", "ext"})
    {
        INFO(id);
        REQUIRE(ui::findEffectIcon(id) != nullptr);
        for (const float size : {24.0f, 40.0f, 48.0f})
        {
            CHECK(ui::EffectIcon::primitives(id, size) > 0);
            CHECK(ui::EffectIcon::primitives(id, size) < 400); // SELECT FX: 15 icons stay cheap to append
        }
    }
    Env e;
    CHECK(ui::iconIdFor("delayBus") == "delay"); // EffectUiSpec::icon of the fixture
    CHECK(ui::iconIdFor("tremolo") == "ext");    // no drawing of its own yet
    CHECK(ui::iconIdFor("") == "none");
    // Every primitive stays inside the icon's square (round caps included).
    ui::DrawList dl;
    for (const auto& icon : ui::effectIcons())
    {
        dl.clear();
        ui::EffectIcon::draw(dl, icon.id, 100, 100, 48, 0xFFFFFFFF);
        for (const auto& c : dl.commands())
        {
            INFO(icon.id);
            CHECK(c.rect.x >= 100 - 1.5f);
            CHECK(c.rect.y >= 100 - 1.5f);
            CHECK(c.rect.x + c.rect.w <= 148 + 1.5f);
            CHECK(c.rect.y + c.rect.h <= 148 + 1.5f);
        }
    }
}

TEST_CASE("fxui.visual_kinds: every visual kind of the spec has a renderer the panel picks from the descriptor",
          "[fxui]")
{
    Env e;
    for (const auto kind : {"lfoCurve", "eqCurve", "filterCurve", "delayTaps", "decayCurve", "meters", "duckEnvelope"})
    {
        INFO(kind);
        const auto make = ui::WidgetRegistry::builtin().find(std::string("fxvisual.") + kind);
        REQUIRE(make != nullptr);
        auto w = make(ui::ScreenSpec{}, ui::WidgetArgs{ui::fxzone::zone});
        auto* v = dynamic_cast<ui::FxVisual*>(w.get());
        REQUIRE(v != nullptr);
        CHECK(v->kind() == kind);
        CHECK(v->bounds().x == 32);
        CHECK(v->bounds().w == 1216);
        CHECK(ui::defaultVisualAdapter(kind) != nullptr);
    }
    const std::pair<std::string_view, std::string_view> byEffect[] = {
        {"chorus", "lfoCurve"}, {"eq2", "eqCurve"},       {"filter", "filterCurve"},
        {"delay", "delayTaps"}, {"compressor", "meters"}, {"ducker", "duckEnvelope"}};
    auto& panel = e.open("track.fx1");
    for (const auto& [effect, kind] : byEffect)
    {
        e.world.slots.selectEffect(fx::Slot::Fx1, 1, 5, effect);
        e.f.frame();
        REQUIRE(panel.visual() != nullptr);
        CHECK(panel.visual()->kind() == kind);
    }
    e.world.slots.selectEffect(fx::Slot::Fx1, 1, 5, "lofi"); // visual none: zone not drawn
    e.f.frame();
    CHECK(panel.visual() == nullptr);
    e.world.slots.selectEffect(fx::Slot::Send, 1, 0, "reverb");
    auto& bus = e.open("mixer.busFx", {}, fx::Slot::Send);
    REQUIRE(bus.visual() != nullptr);
    CHECK(bus.visual()->kind() == "decayCurve");
}

TEST_CASE("fxui.visual_redraw: a knob turn re-evaluates the curve within the frame; idle frames reuse it (UID-835)",
          "[fxui]")
{
    Env e;
    auto& panel = e.open("track.fx1", "filter");
    REQUIRE(panel.visual() != nullptr);
    const float before = panel.visual()->model().cutoffHz;
    auto* v = const_cast<ui::FxVisual*>(panel.visual());
    CHECK_FALSE(v->setModel(v->model())); // unchanged parameters: the cached geometry is reused
    e.f.turn(io::knob(1), -20);
    e.f.frame();
    CHECK(panel.visual()->model().cutoffHz < before);
    // TYPE OFF draws a flat response without the cut-off line.
    e.f.set("fx1.filter.type", 3);
    e.f.frame();
    CHECK(panel.visual()->model().filterType == 3);
}

namespace
{
fx::FxMeterView gMeter;
const fx::FxMeterView* meterSource(fx::Slot, int, void*)
{
    return &gMeter;
}
} // namespace

TEST_CASE("fxui.visual_live: the meters and the duck dot follow the live feed and animate while playing (UID-834)",
          "[fxui]")
{
    auto env = fxtest::World::get().environment();
    env.meters = &meterSource;
    fx::ScopedFxEnvironment scoped(env);
    fxtest::FxShell f(fixedMeasure());
    fxtest::World::get().slots.reset();
    f.shell->state().selectedTrack = 5;
    fxtest::World::get().slots.selectEffect(fx::Slot::Fx1, 1, 5, "compressor");
    REQUIRE(f.shell->open("track.fx1"));
    f.settle();
    auto* panel = f.panel();
    REQUIRE(panel != nullptr);
    gMeter.in = {0.7f, 0.6f};
    gMeter.gainReduction = 0.25f;
    f.shell->state().playing = true;
    f.frame();
    CHECK(panel->visual()->model().in[0] == 0.7f);
    CHECK(panel->visual()->model().gainReduction == 0.25f);
    CHECK(panel->dirty(f.now)); // meters animate while playing
    f.shell->state().playing = false;
    f.frame();
    CHECK_FALSE(panel->dirty(f.now));
}

TEST_CASE("fxui.alloc: the effect panel frame and hot input allocate nothing after warm-up", "[fxui][alloc]")
{
    auto env = fxtest::World::get().environment();
    env.meters = &meterSource;
    fx::ScopedFxEnvironment scoped(env);
    fxtest::FxShell f(fixedMeasure());
    auto& w = fxtest::World::get();
    w.slots.reset();
    f.shell->state().selectedTrack = 5;
    f.shell->state().playing = true;
    for (const auto* effect : {"filter", "chorus", "compressor", "ducker", "eq2", "delay"})
    {
        w.slots.selectEffect(fx::Slot::Fx1, 1, 5, effect);
        REQUIRE(f.shell->open("track.fx1"));
        for (int i = 0; i < 6; ++i)
        {
            f.turn(io::knob(1 + i % 6), i % 2 ? 3 : -3);
            f.frame();
        }
        const std::size_t before = core::rt::violationCount();
        core::rt::setAbortOnViolation(false);
        {
            core::rt::ScopedRealtimeThread rt;
            for (int i = 0; i < 30; ++i)
            {
                gMeter.in = {0.1f * static_cast<float>(i % 10), 0.5f};
                gMeter.duckEnvelope = 0.03f * static_cast<float>(i);
                f.turn(io::knob(1 + i % 6), i % 2 ? 1 : -1);
                f.turn(io::Control::Selector, i % 3 ? 1 : -1);
                f.press(io::step(1 + i % 16));
                f.turn(io::knob(2), 1);
                f.release(io::step(1 + i % 16));
                f.frame();
            }
        }
        core::rt::setAbortOnViolation(true);
        INFO(effect);
        CHECK(core::rt::violationCount() - before == 0);
        f.press(io::Control::Back);
        f.settle();
    }
}
