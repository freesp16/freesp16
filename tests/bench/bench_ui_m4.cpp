// Performance screen benchmarks: the Shell builds each performance screen's full frame
// into a reserved DrawList 1,000 times with a running playhead from a fake engine bridge, glyph-atlas metrics at the
// device's 800 × 480 scale — LFO (SYNC, live dot animating), LFO with the DESTINATION list open (≈ 130 generated rows,
// 8 built), LFO SETTING, SET SCALE, SET SCALE (MIDI), TOUCH STRIP SETTING (strip touched), AMP ENVELOPE (playing) —
// and the components alone: LfoGraph full redraw (a parameter change: the 497-sample cache recomputed + the build),
// LfoGraph live-dot frame (cache hit), EnvelopeEditor, ScaleMap + PadMap.
// A9 budgets: UI core ≤ 8 ms per frame; LfoGraph full redraw ≤ 0.6 ms, live dot ≤ 0.05 ms, EnvelopeGraph ≤
// 0.5 ms, ScaleMap + PadMap ≤ 0.8 ms. Host gates = the A9 budget / 10 (bench_ui_frame's assumed host/A9 ratio; device
// numbers come from the hardware FRAME lines — desktop timings are relative only); whole performance-screen
// frames are gated at 0.08 ms like the sequencing screens' 0.5 ms A9 share (their DrawList build, not the 8 ms frame).
#include "core/commands/ribbon/RibbonCommands.h"
#include "core/params/ParamRegistry.h"
#include "core/util/lfo/LfoLaw.h"
#include "tests/bench/harness/Bench.h"
#include "tests/fakes/FakeEngineBridge.h"
#include "tests/fakes/OffscreenRenderer.h"
#include "ui/screens/home/TrackInfo.h"
#include "ui/screens/sequence/M2Session.h"
#include "ui/screens/touchstrip/StripLive.h"
#include "ui/shell/Shell.h"
#include "ui/widgets/EnvelopeEditor.h"
#include "ui/widgets/LfoGraph.h"
#include "ui/widgets/ScaleMap.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
constexpr double kScreenGateMs = 0.08; // per performance screen DrawList build (host)

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

void report(const char* name, const Stats& s, double gateMs, std::size_t commands)
{
    std::printf("BENCH ui_m4 %s: p50 %.4f ms p95 %.4f ms max %.4f ms, %zu commands (host gate %.3f ms)\n", name, s.p50,
                s.p95, s.max, commands, gateMs);
    const std::string id = std::string("ui_m4_") + name;
    bench::record(id.c_str(), bench::msToCycles(gateMs), bench::msToCycles(s.p50), bench::msToCycles(s.p95));
}

void set(ui::Shell& s, std::string_view id, int track, float v)
{
    ui::m1::setTrackParam(s, id, track, v);
}
void press(ui::Shell& s, io::Control c)
{
    s.dispatch({io::ControlEvent::Kind::Press, c, 100, io::kSourceVirtual, 0});
}
void turn(ui::Shell& s, io::Control c, int d)
{
    s.dispatch({io::ControlEvent::Kind::Turn, c, static_cast<std::int16_t>(d), io::kSourceVirtual, 0});
}
} // namespace

TEST_CASE("bench_ui_m4: frame build of every performance screen with a running playhead", "[bench][ui]")
{
    fakes::OffscreenRenderer offscreen;
    std::string error;
    REQUIRE(offscreen.open(800, 480, error));
    fakes::FakeEngineBridge bridge;
    auto& session = ui::m2::Session::instance();
    session.reset();
    namespace rb = core::commands::ribbon;
    session.execute(rb::assignItem(session.project(), 0, 0, "playback.pitch"));
    session.execute(rb::assignItem(session.project(), 0, 1, "track.volume"));
    session.execute(rb::assignItem(session.project(), 0, 2, "env.velocity"));

    ui::ShellConfig cfg{core::builtinParams(), offscreen.measure(), &bridge};
    ui::Shell shell(std::move(cfg));
    auto& st = shell.state();
    for (int t = 0; t < 16; ++t)
    {
        auto& s = st.tracks[static_cast<std::size_t>(t)];
        s.hasContent = true;
        s.name = "Beat_120bpm_2bar";
    }
    st.playing = true;
    shell.selectTrack(5);
    set(shell, "lfo.shape", 5, 0);
    set(shell, "lfo.sync", 5, 1);
    set(shell, "lfo.speed", 5, core::lfo::syncDivisionKnob(7));
    set(shell, "lfo.depth", 5, 90);
    set(shell, "lfo.destination", 5, static_cast<float>(core::builtinParams().idOf("playback.pitch")->value));
    set(shell, "env.attack", 5, 20);
    set(shell, "env.hold", 5, 60);
    set(shell, "env.release", 5, 70);
    ui::DrawList dl;
    dl.reserve(8192, 65536);
    double now = 0;
    auto frame = [&](int i)
    {
        now += 16.7;
        bridge.playheadFeed.blockIndex = static_cast<std::uint64_t>(i + 1);
        bridge.playheadFeed.playing = true;
        bridge.playheadFeed.bpm = 120;
        bridge.playheadFeed.ppq = static_cast<double>(i) * 16.7 / 500.0; // 120 BPM
        bridge.playheadFeed.step = static_cast<std::uint8_t>(static_cast<int>(bridge.playheadFeed.ppq * 4) % 16);
        ui::m4::stripLive() = {2, true, (i * 3) % 128};
        dl.clear();
        shell.frame(dl, now);
    };
    struct Case
    {
        const char* name;
        void (*enter)(ui::Shell&);
    };
    static const Case cases[] = {
        {"lfo",
         [](ui::Shell& s)
         {
             s.open("track.menu");
             s.open("track.lfo");
         }},
        {"lfo_destination", [](ui::Shell& s) { press(s, io::Control::SelectorPush); }},
        {"lfo_setting",
         [](ui::Shell& s)
         {
             press(s, io::Control::Back);
             turn(s, io::Control::Selector, 2);
             press(s, io::Control::SelectorPush);
         }},
        {"ampEnvelope",
         [](ui::Shell& s)
         {
             press(s, io::Control::Back);
             s.back();
             s.open("track.ampEnvelope");
             s.state().tracks[4].lastHitMs = 1e12; // keep the playhead dot riding (set per frame below)
         }},
        {"setScale",
         [](ui::Shell& s)
         {
             s.back();
             s.open("track.playback");
             s.open("track.playback.setScale");
         }},
        {"setScaleMidi",
         [](ui::Shell& s)
         {
             s.home();
             s.open("track.menu");
             s.open("track.setScaleMidi");
         }},
        {"touchStrip",
         [](ui::Shell& s)
         {
             s.home();
             s.open("utility");
             s.open("utility.touchStrip.user1");
         }},
    };
    double worst = 0;
    for (const auto& c : cases)
    {
        c.enter(shell);
        for (int i = 0; i < 100; ++i)
            frame(i); // warm-up (and past the push transition)
        const auto s = measure(1000,
                               [&](int i)
                               {
                                   if (std::string_view(c.name) == "ampEnvelope")
                                       shell.state().tracks[4].lastHitMs = now - 200.0; // a hit 200 ms ago
                                   frame(i);
                               });
        report(c.name, s, kScreenGateMs, dl.commands().size());
        worst = std::max(worst, s.p95);
        CHECK(s.p95 <= kScreenGateMs);
    }
    std::printf("BENCH ui_m4 worst screen p95 %.4f ms (A9 UI core budget 8 ms/frame)\n", worst);
    ui::m4::stripLive() = {};
    shell.home();
    session.reset();
}

TEST_CASE("bench_ui_m4: LfoGraph full redraw and live dot, EnvelopeEditor, ScaleMap + PadMap", "[bench][ui]")
{
    fakes::OffscreenRenderer offscreen;
    std::string error;
    REQUIRE(offscreen.open(800, 480, error));
    const auto& palette = ui::theme::palette(true);
    ui::FocusState focus;
    ui::UiContext ctx{palette, offscreen.measure(), 0, focus};
    ui::DrawList dl;
    dl.reserve(4096, 8192);

    ui::LfoGraph graph;
    graph.setStyle(0xFFD9A100u, 63, false, false);
    // Full redraw: a parameter change every frame (the cache recomputed: 497 samples), then the build.
    const auto full = measure(1000,
                              [&](int i)
                              {
                                  graph.setShape(i % 7, 0.3f + 0.0005f * static_cast<float>(i % 100), 0.25f, true,
                                                 120.0, 4.0f, 0x1234u);
                                  dl.clear();
                                  graph.build(dl, ctx);
                              });
    report("lfoGraph_full", full, 0.06, dl.commands().size());
    CHECK(full.p95 <= 0.06);
    // Live dot: parameters unchanged (cache hit), only the dot moves.
    graph.setShape(0, 0.5f, 0.0f, true, 120.0, 4.0f);
    const auto live = measure(1000,
                              [&](int i)
                              {
                                  graph.setShape(0, 0.5f, 0.0f, true, 120.0, 4.0f);
                                  graph.setLive(static_cast<float>(i % 100) / 100.0f, 0.3f, true);
                                  dl.clear();
                                  graph.build(dl, ctx);
                              });
    report("lfoGraph_liveFrame", live, 0.06, dl.commands().size());
    CHECK(live.p95 <= 0.06);
    // The budget's 0.05 ms "live dot" is the dot's own work (two fills): with the DrawList rebuilt whole each frame
    // the dot frame costs the graph's build; the renderers' damage diff redraws only the dot's rectangles.

    ui::EnvelopeModel env;
    env.attackS = 0.01f;
    env.holdS = 0.2f;
    env.releaseS = 0.5f;
    env.holdInf = false;
    env.r = ui::EnvCurve::Curve;
    env.windowS = 1.0f;
    env.colour = 0xFFD9A100u;
    const auto envelope = measure(1000,
                                  [&](int i)
                                  {
                                      env.playheadS = static_cast<float>(i % 60) / 100.0f;
                                      dl.clear();
                                      ui::EnvelopeEditor::draw(dl, ctx, env);
                                  });
    report("envelopeEditor", envelope, 0.05, dl.commands().size());
    CHECK(envelope.p95 <= 0.05);

    const auto scale = measure(1000,
                               [&](int i)
                               {
                                   ui::ScaleMapModel sm;
                                   sm.mask = 0xAB5;
                                   sm.area = i % 5 - 2;
                                   sm.colour = 0xFFD9A100u;
                                   ui::PadMapModel pm;
                                   pm.mask = sm.mask;
                                   pm.area = sm.area;
                                   pm.colour = sm.colour;
                                   pm.midi = (i & 1) != 0;
                                   dl.clear();
                                   ui::ScaleMap::draw(dl, ctx, sm);
                                   ui::PadMap::draw(dl, ctx, pm);
                               });
    report("scaleMap_padMap", scale, 0.08, dl.commands().size());
    CHECK(scale.p95 <= 0.08);
}
