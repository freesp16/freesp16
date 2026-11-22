// Sample-playback screen frame-build benchmark: the Shell builds each such screen's
// full frame (status row, context bar, body, rail, knob strip) into a reserved DrawList 1,000 times with live meters
// (every track, SEND, MASTER) and a moving playhead from a fake engine bridge, glyph-atlas metrics at the device's
// 800 × 480 scale. Reports p50 / p95 / max and the command count per screen.
// Budget per screen on the Cortex-A9: ≤ 0.5 ms DrawList build (the whole-frame build budget is
// 2 ms). Host gate: p95 ≤ 0.05 ms = the A9 budget with the 10× host/A9 ratio bench_ui_frame assumes (our choice;
// device numbers come from the emulator / hardware FRAME lines, emulator timings are relative only).
#include "core/params/ParamRegistry.h"
#include "tests/bench/harness/Bench.h"
#include "tests/fakes/FakeEngineBridge.h"
#include "tests/fakes/OffscreenRenderer.h"
#include "tests/snapshot/m1/DemoSample.h"
#include "tests/snapshot/m1/FakeLibrary.h"
#include "ui/screens/home/TrackInfo.h"
#include "ui/shell/Shell.h"
#include "ui/widgets/Meter.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
constexpr double kHostGateMs = 0.05;

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
} // namespace

TEST_CASE("bench_ui_m1: frame build of every sample-playback screen with live meters", "[bench][ui]")
{
    fakes::OffscreenRenderer offscreen;
    std::string error;
    REQUIRE(offscreen.open(800, 480, error));
    fakes::FakeEngineBridge bridge;
    m1test::DemoSample beat{"beat"};
    m1test::FakeLibrary lib;
    ui::m1::SampleViews::instance().setProvider([&](int) { return beat.view("Beat_120bpm_2bar"); });
    ui::m1::BrowseSource::install(&lib);

    ui::ShellConfig cfg{core::builtinParams(), offscreen.measure(), &bridge};
    ui::Shell shell(std::move(cfg));
    auto& st = shell.state();
    for (int t = 0; t < 16; ++t)
    {
        auto& s = st.tracks[static_cast<std::size_t>(t)];
        s.hasContent = t % 3 != 2;
        s.name = "Beat_120bpm_2bar";
        s.stretchBadge = "M.TMP";
        s.loopGlyph = "\xE2\x9F\xB2";
    }
    st.selectedTrack = 5;
    st.playing = true;
    ui::m1::setTrackParam(shell, "playback.loop", 5, 1);
    ui::m1::setTrackParam(shell, "env.hold", 5, 60);
    ui::m1::setTrackParam(shell, "env.release", 5, 70);
    ui::m1::setTrackParam(shell, "env.releaseCurve", 5, 1);
    ui::DrawList dl;
    dl.reserve(8192, 65536);
    double now = 0;
    auto frame = [&](int i)
    {
        now += 16.7;
        for (int t = 0; t < 16; ++t)
        {
            const float v = 0.5f * std::fabs(std::sin(static_cast<float>(i) * 0.21f + static_cast<float>(t)));
            bridge.meterFeed.trackPeak[static_cast<std::size_t>(t)] = {v, v * 0.9f};
        }
        bridge.meterFeed.masterPeak = {0.6f, 0.55f};
        bridge.meterFeed.sendPeak = {0.3f, 0.25f};
        bridge.playheadFeed.trackPosition.fill(-1);
        bridge.playheadFeed.trackPosition[4] = static_cast<float>(i % 240) / 240.0f;
        if (i % 4 == 0)
            shell.dispatch({io::ControlEvent::Kind::Press, io::pad(1 + i % 16), 100, io::kSourceVirtual, now});
        dl.clear();
        shell.frame(dl, now);
    };
    static constexpr const char* screens[] = {"home",           "trackSetting",      "mixer",       "track.menu",
                                              "track.playback", "track.ampEnvelope", "track.browse"};
    double worst = 0;
    for (const auto* id : screens)
    {
        if (std::string_view(id) != "home")
            REQUIRE(shell.open(id));
        for (int i = 0; i < 100; ++i)
            frame(i); // warm-up (and past the push transition)
        st.selectedTrack = 5;
        const auto s = measure(1000, frame);
        std::printf("BENCH ui_m1 %s: p50 %.4f ms p95 %.4f ms max %.4f ms, %zu commands (host gate %.3f ms, A9 "
                    "budget 0.5 ms)\n",
                    id, s.p50, s.p95, s.max, dl.commands().size(), kHostGateMs);
        std::string name = std::string("ui_m1_") + id;
        std::replace(name.begin(), name.end(), '.', '_');
        bench::record(name.c_str(), bench::msToCycles(kHostGateMs), bench::msToCycles(s.p50), bench::msToCycles(s.p95));
        worst = std::max(worst, s.p95);
        CHECK(s.p95 <= kHostGateMs);
    }
    std::printf("BENCH ui_m1 worst p95 %.4f ms\n", worst);
    ui::m1::BrowseSource::install(nullptr);
    ui::m1::SampleViews::instance().setProvider({});
}
