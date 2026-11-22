// Sequencing screen frame-build benchmark: the Shell builds each such screen's
// full frame (status row, context bar, body, knob strip, popups) into a reserved DrawList 1,000 times with a running
// playhead from a fake engine bridge (step advancing every 4 frames, live meters), glyph-atlas metrics at the device's
// 800 × 480 scale: SEQUENCE (16 steps with offsets, retriggers and a target), SCENE MANAGER PATTERN / SEQ. / TRACK
// (content in 4 scenes, a full SEQ. overview), BPM/QUANTIZE over HOME, PROJECT, OPEN PROJECT (40 projects), the
// SAVE AS dialog with the keyboard, RENDER AUDIO. Reports p50 / p95 / max and the command count per screen.
// Budget per screen on the Cortex-A9: ≤ 0.5 ms DrawList build (the whole-frame build budget is
// 2 ms). Host gate: p95 ≤ 0.05 ms = the A9 budget with the 10× host/A9 ratio bench_ui_frame assumes (our choice;
// device numbers come from the emulator / hardware FRAME lines, emulator timings are relative only).
#include "core/params/ParamRegistry.h"
#include "tests/bench/harness/Bench.h"
#include "tests/fakes/FakeEngineBridge.h"
#include "tests/fakes/OffscreenRenderer.h"
#include "tests/snapshot/m2/FakeProjectSource.h"
#include "ui/screens/home/TrackInfo.h"
#include "ui/screens/project/ProjectAccess.h"
#include "ui/screens/project/ProjectSource.h"
#include "ui/screens/scenemanager/SceneManagerAccess.h"
#include "ui/screens/sequence/M2Commands.h"
#include "ui/screens/sequence/M2Session.h"
#include "ui/shell/Feeds.h"
#include "ui/shell/Shell.h"

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

void demo(ui::m2::Session& s)
{
    using core::model::TriggerType;
    s.execute(ui::m2::cmd::setPatternLength(s.project(), 0, 0, 64));
    for (int t = 0; t < 16; ++t)
        for (int st = t % 3; st < 64; st += 2 + t % 3)
            s.execute(ui::m2::cmd::toggleStep(
                s.project(), 0, 0, t, st,
                st % 5 == 0 ? TriggerType::Half : (st % 7 == 0 ? TriggerType::LockOnly : TriggerType::Full), 127));
    for (int st = 0; st < 16; ++st)
    {
        const int steps[] = {st};
        const float off[] = {static_cast<float>((st % 9) - 4)}, cnt[] = {static_cast<float>(1 + st % 4)};
        s.execute(ui::m2::cmd::setLocks(s.project(), 0, 0, 0, steps, ui::m2::kSlotOffset, off, 0, "OFF SET"));
        s.execute(ui::m2::cmd::setLocks(s.project(), 0, 0, 0, steps, ui::m2::kSlotRetrigCount, cnt, 1, "CNT"));
    }
    for (int sc = 1; sc < 4; ++sc)
        s.execute(ui::m2::cmd::toggleStep(s.project(), sc, 0, 0, 0, TriggerType::Full, 127));
    s.execute(ui::m2::cmd::setSceneName(0, "Intro"));
}
} // namespace

TEST_CASE("bench_ui_m2: frame build of every sequencing screen with a running playhead", "[bench][ui]")
{
    fakes::OffscreenRenderer offscreen;
    std::string error;
    REQUIRE(offscreen.open(800, 480, error));
    fakes::FakeEngineBridge bridge;
    auto& session = ui::m2::Session::instance();
    session.reset();
    ui::resetSceneManager();
    demo(session);
    auto& projects = m2test::FakeProjectSource::instance();
    projects.reset();
    ui::m2::ProjectSource::install(&projects);
    for (int i = 0; i < 40; ++i)
    {
        ui::m2::ProjectEntry e;
        e.name = "Project_" + std::to_string(100 + i);
        e.modified = "12 Nov 2026";
        e.modifiedKey = 1000 - i;
        e.scenes = i % 16;
        e.external = i % 5 == 0;
        e.location = e.external ? "USB" : "";
        projects.addEntry(std::move(e));
    }
    projects.setCurrent("Project_101", true);

    ui::ShellConfig cfg{core::builtinParams(), offscreen.measure(), &bridge};
    cfg.flags.enable(ui::m2::kParamStubFlag);
    cfg.flags.enable(ui::m2::kRenderFlag);
    ui::Shell shell(std::move(cfg));
    auto& st = shell.state();
    for (int t = 0; t < 16; ++t)
    {
        auto& s = st.tracks[static_cast<std::size_t>(t)];
        s.hasContent = t % 3 != 2;
        s.name = "Beat_120bpm_2bar";
        s.loopGlyph = t % 4 == 0 ? "\xE2\x9F\xB2" : "\xE2\x86\x92";
    }
    st.playing = true;
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
        bridge.playheadFeed.blockIndex = static_cast<std::uint64_t>(i + 1);
        bridge.playheadFeed.playing = true;
        bridge.playheadFeed.step = static_cast<std::uint8_t>((i / 4) % 16);
        bridge.playheadFeed.ppq = static_cast<double>(i / 4 % 16) * 0.25 + 0.0625 * (i % 4);
        bridge.playheadFeed.pendingPattern = static_cast<std::int8_t>(i % 64 < 32 ? 2 : -1);
        dl.clear();
        shell.frame(dl, now);
    };
    struct Case
    {
        const char* name;
        void (*enter)(ui::Shell&);
    };
    static const Case cases[] = {
        {"sequence",
         [](ui::Shell& s)
         {
             s.open("track.menu");
             s.open("track.sequence");
             s.dispatch({io::ControlEvent::Kind::Press, io::step(3), 100, io::kSourceVirtual, 0});
             s.dispatch({io::ControlEvent::Kind::Release, io::step(3), 0, io::kSourceVirtual, 0});
         }},
        {"sceneManager_pattern", [](ui::Shell& s) { s.open("sceneManager"); }},
        {"sceneManager_seq",
         [](ui::Shell& s)
         {
             io::ControlEvent e{io::ControlEvent::Kind::Touch, io::Control::Screen, ui::touch::kDown,
                                io::kSourceVirtual, 0};
             e.x = 70;
             e.y = 200;
             s.dispatch(e);
         }},
        {"sceneManager_track",
         [](ui::Shell& s)
         {
             io::ControlEvent e{io::ControlEvent::Kind::Touch, io::Control::Screen, ui::touch::kDown,
                                io::kSourceVirtual, 0};
             e.x = 70;
             e.y = 274;
             s.dispatch(e);
         }},
        {"bpmQuantize",
         [](ui::Shell& s)
         {
             io::ControlEvent e{io::ControlEvent::Kind::Touch, io::Control::Screen, ui::touch::kDown,
                                io::kSourceVirtual, 0};
             e.x = 70;
             e.y = 140; // back to the PATTERN tab first (the tab is remembered)
             s.dispatch(e);
             s.home();
             s.open("bpmQuantize");
         }},
        {"project",
         [](ui::Shell& s)
         {
             s.back();
             s.open("project");
         }},
        {"project_open", [](ui::Shell& s) { s.open("project.open"); }},
        {"project_saveAs",
         [](ui::Shell& s)
         {
             s.back();
             s.dispatch({io::ControlEvent::Kind::Turn, io::Control::Selector, 3, io::kSourceVirtual, 0});
             s.dispatch({io::ControlEvent::Kind::Press, io::Control::SelectorPush, 100, io::kSourceVirtual, 0});
         }},
        {"project_renderAudio",
         [](ui::Shell& s)
         {
             s.back();
             s.open("project.renderAudio");
         }},
    };
    double worst = 0;
    for (const auto& c : cases)
    {
        c.enter(shell);
        for (int i = 0; i < 100; ++i)
            frame(i); // warm-up (and past the push transition)
        const auto s = measure(1000, frame);
        std::printf("BENCH ui_m2 %s: p50 %.4f ms p95 %.4f ms max %.4f ms, %zu commands (host gate %.3f ms, A9 "
                    "budget 0.5 ms)\n",
                    c.name, s.p50, s.p95, s.max, dl.commands().size(), kHostGateMs);
        const std::string name = std::string("ui_m2_") + c.name;
        bench::record(name.c_str(), bench::msToCycles(kHostGateMs), bench::msToCycles(s.p50), bench::msToCycles(s.p95));
        worst = std::max(worst, s.p95);
        CHECK(s.p95 <= kHostGateMs);
    }
    std::printf("BENCH ui_m2 worst p95 %.4f ms\n", worst);
    shell.home();
    session.reset();
    projects.reset();
    ui::m2::ProjectSource::install(nullptr);
    ui::resetProjectScreens();
}
