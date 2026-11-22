// Frame-build benchmark: the Shell builds a full HOME frame (status row,
// context bar, 16 tiles, meter, knob strip) — and the same with a ListPopup + toast open — into a reserved DrawList,
// with glyph-atlas text metrics. Budget on the Cortex-A9: ≤ 0.5 ms per full frame build. Host gate: p95 ≤ 0.05 ms,
// i.e. the A9 budget with a 10× host/A9 speed ratio (our assumption; the device number comes from the emulator /
// hardware FRAME lines, emulator timings are relative only).
#include "core/params/ParamRegistry.h"
#include "tests/fakes/FakeEngineBridge.h"
#include "tests/fakes/OffscreenRenderer.h"
#include "ui/shell/Shell.h"
#include "tests/bench/harness/Bench.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
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

TEST_CASE("ui_frame: full frame build of HOME through the Shell", "[bench][ui]")
{
    fakes::OffscreenRenderer offscreen;
    std::string error;
    REQUIRE(offscreen.open(800, 480, error)); // the device's atlas scale (0.625)
    fakes::FakeEngineBridge bridge;
    bridge.meterFeed.masterPeak = {0.5f, 0.4f};
    ui::ScreenRegistry screens;
    ui::ShellConfig cfg{core::builtinParams(), offscreen.measure(), &bridge};
    cfg.screens = &screens;
    ui::Shell shell(std::move(cfg));
    auto& st = shell.state();
    for (int t = 0; t < 16; ++t)
    {
        st.tracks[static_cast<std::size_t>(t)].hasContent = t % 3 != 2;
        st.tracks[static_cast<std::size_t>(t)].name = "Beat_120bpm_2bar";
    }
    ui::DrawList dl;
    dl.reserve(4096, 32768);
    double now = 0;
    auto frame = [&](int i)
    {
        now += 16.7;
        if (i % 4 == 0) // live input every few frames: pad flash + encoder underline
        {
            shell.dispatch({io::ControlEvent::Kind::Press, io::pad(1 + i % 16), 100, io::kSourceVirtual, now});
            shell.dispatch({io::ControlEvent::Kind::Turn, io::knob(1 + i % 6), 1, io::kSourceVirtual, now});
        }
        dl.clear();
        shell.frame(dl, now);
    };
    for (int i = 0; i < 200; ++i)
        frame(i); // warm-up
    const auto home = measure(5000, frame);
    std::printf("BENCH ui_frame home: p50 %.4f ms p95 %.4f ms max %.4f ms, %zu commands (host gate %.3f ms, A9 budget "
                "0.5 ms)\n",
                home.p50, home.p95, home.max, dl.commands().size(), kHostGateMs);

    ui::ListRequest r;
    r.title = "DESTINATION";
    for (int i = 0; i < 20; ++i)
        r.items.push_back({"PITCH", i < 10 ? "PLAYBACK" : "AMP", {}, i});
    r.grouped = true;
    shell.showList(r);
    ui::ToastRequest toast;
    toast.text = "Saved \"Live set 3\"";
    shell.showToast(toast);
    for (int i = 0; i < 50; ++i)
        frame(i);
    const auto popup = measure(5000, frame);
    std::printf("BENCH ui_frame home+list+toast: p50 %.4f ms p95 %.4f ms max %.4f ms, %zu commands\n", popup.p50,
                popup.p95, popup.max, dl.commands().size());
    bench::record("ui_frame_home", bench::msToCycles(kHostGateMs), bench::msToCycles(home.p50), bench::msToCycles(home.p95));
    bench::record("ui_frame_home_popup", bench::msToCycles(kHostGateMs), bench::msToCycles(popup.p50),
                  bench::msToCycles(popup.p95));
    CHECK(home.p95 <= kHostGateMs);
    CHECK(popup.p95 <= kHostGateMs);
}
