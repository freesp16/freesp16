// Effect UI benchmarks. Budgets on the Cortex-A9 and the host gates at the
// assumed 10× host/A9 speed ratio of bench_ui_frame (device numbers come from the emulator / hardware FRAME lines;
// emulator timings are relative only):
//   ui_effects_frame       full frame through the Shell with FX1 = MULTI MODE FILTER (panel, curve, six glyphs, strip)
//                          A9 ≤ 0.5 ms (UI-core frame rule; the panel-only figure is 0.8 ms) → host ≤ 0.05 ms
//   ui_effects_frame_live  FX1 = COMPRESSOR, playing, live meters changing every frame                → same gate
//   ui_effects_knob_turn   encoder turn + the frame that repaints glyph, column and curve (256-point re-evaluation)
//                          A9 ≤ 0.4 ms                                                                → host ≤ 0.04 ms
//   ui_effects_curve       filterCurve / eqCurve 256-point evaluation on a parameter change: A9 ≤ 0.3 ms → ≤ 0.03 ms
//   ui_effects_select_open SELECT FX: request from the registry + GridPopup + first frame: A9 ≤ 2 ms   → ≤ 0.2 ms
// The `soft` rasterisation of each state (800 × 480, the CPU fallback backend) is measured and printed, not gated:
// the device draws with the GC2000. Gates use the median (the harness's budget rule, Bench.h): p95 on this
// shared host swings with the other jobs; both are printed and recorded.
#include "tests/bench/harness/Bench.h"
#include "tests/fakes/OffscreenRenderer.h"
#include "tests/snapshot/m3/FxFixture.h"
#include "ui/widgets/FxGrid.h"
#include "ui/widgets/GridPopup.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

namespace
{
namespace fx = ui::fx;

struct Stats
{
    double p50, p95, max;
};

template <typename F> Stats measure(int iterations, F&& f)
{
    std::vector<double> ms;
    ms.reserve(static_cast<std::size_t>(iterations));
    for (int i = 0; i < iterations; ++i)
    {
        const auto t0 = std::chrono::steady_clock::now();
        f(i);
        ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    std::sort(ms.begin(), ms.end());
    return {ms[ms.size() / 2], ms[ms.size() * 95 / 100], ms.back()};
}

void report(const char* name, const Stats& s, double gateMs, double a9Ms, std::size_t commands)
{
    std::printf("BENCH %s: p50 %.4f ms p95 %.4f ms max %.4f ms, %zu commands (host gate %.3f ms, A9 budget %.2f ms)\n",
                name, s.p50, s.p95, s.max, commands, gateMs, a9Ms);
    bench::record(name, bench::msToCycles(gateMs), bench::msToCycles(s.p50), bench::msToCycles(s.p95));
}

fx::FxMeterView gMeter;
const fx::FxMeterView* meters(fx::Slot, int, void*)
{
    return &gMeter;
}

struct Rig
{
    fakes::OffscreenRenderer offscreen;
    std::unique_ptr<fx::ScopedFxEnvironment> env;
    std::unique_ptr<fxtest::FxShell> f;
    Rig()
    {
        std::string error;
        REQUIRE(offscreen.open(800, 480, error)); // the device's atlas scale (0.625)
        auto e = fxtest::World::get().environment();
        e.meters = &meters;
        env = std::make_unique<fx::ScopedFxEnvironment>(e);
        fxtest::World::get().slots.reset();
        f = std::make_unique<fxtest::FxShell>(offscreen.measure());
        f->shell->state().selectedTrack = 5;
    }
    void open(std::string_view effect)
    {
        fxtest::World::get().slots.selectEffect(fx::Slot::Fx1, 1, 5, effect);
        REQUIRE(f->shell->open("track.fx1"));
        for (int i = 0; i < 60; ++i)
            f->frame(16.7); // warm-up: transitions end, caches fill
    }
    double softMs()
    {
        const auto s = measure(30, [&](int) { offscreen.draw(f->dl); });
        return s.p50;
    }
};
} // namespace

TEST_CASE("bench_ui_effects: frame build with the effect panel open", "[bench][ui]")
{
    constexpr double kGate = 0.05;
    Rig rig;
    rig.open("filter");
    auto& f = *rig.f;
    const auto still = measure(4000, [&](int) { f.frame(16.7); });
    report("ui_effects_frame", still, kGate, 0.5, f.dl.commands().size());
    std::printf("BENCH ui_effects_frame soft raster 800x480: %.3f ms\n", rig.softMs());
    CHECK(still.p50 <= kGate);

    rig.open("compressor");
    f.shell->state().playing = true;
    const auto live = measure(4000,
                              [&](int i)
                              {
                                  gMeter.in = {0.05f * static_cast<float>(i % 20), 0.6f};
                                  gMeter.out = {0.5f, 0.04f * static_cast<float>(i % 25)};
                                  gMeter.gainReduction = 0.01f * static_cast<float>(i % 50);
                                  f.frame(16.7);
                              });
    report("ui_effects_frame_live", live, kGate, 0.5, f.dl.commands().size());
    std::printf("BENCH ui_effects_frame_live soft raster 800x480: %.3f ms\n", rig.softMs());
    CHECK(live.p50 <= kGate);
    f.shell->state().playing = false;

    rig.open("chorus");
    const auto lfo = measure(2000, [&](int) { f.frame(16.7); });
    std::printf("BENCH ui_effects_frame chorus (lfoCurve): p50 %.4f ms p95 %.4f ms, %zu commands\n", lfo.p50, lfo.p95,
                f.dl.commands().size());
}

TEST_CASE("bench_ui_effects: knob turn repaint and curve evaluation", "[bench][ui]")
{
    Rig rig;
    rig.open("filter");
    auto& f = *rig.f;
    const auto turn = measure(4000,
                              [&](int i)
                              {
                                  f.turn(io::knob(1), i % 2 ? 1 : -1); // CUT OFF: the curve re-evaluates
                                  f.frame(16.7);
                              });
    report("ui_effects_knob_turn", turn, 0.04, 0.4, f.dl.commands().size());
    CHECK(turn.p50 <= 0.04);

    auto make = ui::WidgetRegistry::builtin().find("fxvisual.filterCurve");
    auto w = make(ui::ScreenSpec{}, ui::WidgetArgs{ui::fxzone::zone});
    auto* filter = dynamic_cast<ui::FxVisual*>(w.get());
    auto eqw =
        ui::WidgetRegistry::builtin().find("fxvisual.eqCurve")(ui::ScreenSpec{}, ui::WidgetArgs{ui::fxzone::zone});
    auto* eq = dynamic_cast<ui::FxVisual*>(eqw.get());
    REQUIRE((filter != nullptr && eq != nullptr));
    ui::VisualModel m;
    const auto curve = measure(4000,
                               [&](int i)
                               {
                                   m.cutoffHz = 100.0f + static_cast<float>(i % 1000) * 10.0f;
                                   m.band[1].freqHz = m.cutoffHz;
                                   filter->setModel(m);
                                   eq->setModel(m);
                               });
    const Stats perCurve{curve.p50 / 2, curve.p95 / 2, curve.max / 2};
    report("ui_effects_curve", perCurve, 0.03, 0.3, 256);
    CHECK(perCurve.p50 <= 0.03);
}

TEST_CASE("bench_ui_effects: opening SELECT FX", "[bench][ui]")
{
    Rig rig;
    rig.open("filter");
    auto& f = *rig.f;
    ui::DrawList dl;
    dl.reserve(8192, 65536);
    const auto& p = ui::theme::palette(true);
    ui::FocusState focus;
    ui::UiContext ctx{p, rig.offscreen.measure(), 0, focus};
    const auto open = measure(1000,
                              [&](int)
                              {
                                  ui::GridPopup grid(ui::FxGrid::request(fx::Slot::Fx1, 5, "filter", {}));
                                  dl.clear();
                                  grid.build(dl, ctx);
                              });
    report("ui_effects_select_open", open, 0.2, 2.0, dl.commands().size());
    CHECK(open.p50 <= 0.2);
    f.dl.clear();
    ui::GridPopup grid(ui::FxGrid::request(fx::Slot::Fx1, 5, "filter", {}));
    grid.build(f.dl, ctx);
    std::printf("BENCH ui_effects_select_open soft raster 800x480: %.3f ms\n", rig.softMs());
}
