// Sampling and sync screen benchmarks: the Shell builds each such screen's full frame into
// a reserved DrawList 1,000 times with a running playhead and live meters, glyph-atlas metrics at the device's 800 × 480
// scale — ARRANGER (999 rows, the song playing), OUTPUT SETTING, SAMPLING/EDIT (a take recording: 735 new ring frames
// per frame), LIVE SAMPLING (the ring scrolling), MIDI NOTE, MIDI CC1, RENDER AUDIO — and the components alone:
// RingView (incremental scroll: fold the new frames + build), the live take fill, PianoKeyboard, six CcCards, DataTable
// with 9 visible of 999 rows.
// A9 budgets: UI core ≤ 8 ms per frame; RingView ≤ 1.0 ms, live take fill ≤ 0.3 ms, PianoKeyboard ≤ 0.2 ms,
// 6 CcCards ≤ 0.5 ms, DataTable 9 rows ≤ 0.6 ms. Host gates = the A9 budget / 10 (bench_ui_frame's assumed host/A9
// ratio; device numbers come from the hardware FRAME lines — desktop timings are relative only); whole frames of these
// screens are gated at 0.08 ms like the performance screens.
#include "core/commands/arrangement/ArrangementCommands.h"
#include "core/params/ParamRegistry.h"
#include "core/util/arrangement/ArrangementPlan.h"
#include "tests/bench/harness/Bench.h"
#include "tests/fakes/FakeEngineBridge.h"
#include "tests/fakes/OffscreenRenderer.h"
#include "tests/snapshot/m5/FakeSamplingHost.h"
#include "ui/screens/home/TrackInfo.h"
#include "ui/screens/sequence/M2Session.h"
#include "ui/shell/Shell.h"
#include "ui/widgets/CcCard.h"
#include "ui/widgets/DataTable.h"
#include "ui/widgets/PianoKeyboard.h"
#include "ui/widgets/RingView.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
constexpr double kScreenGateMs = 0.08; // per sampling/sync screen DrawList build (host)

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
    std::printf("BENCH ui_m5 %s: p50 %.4f ms p95 %.4f ms max %.4f ms, %zu commands (host gate %.3f ms)\n", name, s.p50,
                s.p95, s.max, commands, gateMs);
    const std::string id = std::string("ui_m5_") + name;
    bench::record(id.c_str(), bench::msToCycles(gateMs), bench::msToCycles(s.p50), bench::msToCycles(s.p95));
}

// One 60 Hz frame of input (735 frames at 44.1 kHz) of a decaying burst into the ring.
void feedRing(core::sampling::CaptureRing& ring, int i)
{
    static std::vector<float> buf(735);
    for (std::size_t k = 0; k < buf.size(); ++k)
        buf[k] = 0.6f * std::sin(0.05f * static_cast<float>(k + static_cast<std::size_t>(i) * 735)) *
                 std::exp(-0.002f * static_cast<float>((static_cast<std::size_t>(i) * 735 + k) % 22050));
    ring.write(buf.data(), nullptr, static_cast<int>(buf.size()));
}
} // namespace

TEST_CASE("bench_ui_m5: frame build of every sampling and sync screen with a running playhead and live meters",
          "[bench][ui]")
{
    fakes::OffscreenRenderer offscreen;
    std::string error;
    REQUIRE(offscreen.open(800, 480, error));
    fakes::FakeEngineBridge bridge;
    m5test::FakeSamplingHost host;
    auto& hub = host.hubRef();
    auto& session = ui::m2::Session::instance();
    session.reset();
    for (int r = 0; r < core::commands::arrangement::kMaxRows; ++r) // 999 rows: the table stays virtualised
    {
        core::model::Section s;
        s.scene = static_cast<std::uint8_t>(r % 16);
        s.pattern = static_cast<std::uint8_t>(r % 7);
        s.repeats = 1 + r % 4;
        session.execute(core::commands::arrangement::appendRow(session.project(), s));
    }

    ui::ShellConfig cfg {core::builtinParams(), offscreen.measure(), &bridge};
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
    ui::DrawList dl;
    dl.reserve(8192, 65536);
    double now = 0;
    auto frame = [&](int i)
    {
        now += 16.7;
        bridge.playheadFeed.blockIndex = static_cast<std::uint64_t>(i + 1);
        bridge.playheadFeed.playing = true;
        bridge.playheadFeed.bpm = 120;
        bridge.playheadFeed.ppq = static_cast<double>(i) * 16.7 / 500.0;
        for (int t = 0; t < 16; ++t)
            bridge.meterFeed.trackPeak[static_cast<std::size_t>(t)] = {0.3f + 0.02f * static_cast<float>(i % 10), 0.3f};
        bridge.meterFeed.masterPeak = {0.5f, 0.45f};
        hub.status.peakL = 0.4f + 0.01f * static_cast<float>(i % 20);
        hub.status.peakR = 0.35f;
        feedRing(hub.ring, i);
        core::arrangement::liveSongStatus().row = (i / 50) % 999;
        core::arrangement::liveSongStatus().remaining = 1;
        dl.clear();
        shell.frame(dl, now);
    };
    struct Case
    {
        const char* name;
        void (*enter)(ui::Shell&, core::sampling::CaptureHub&);
    };
    static const Case cases[] = {
        {"arranger", [](ui::Shell& s, core::sampling::CaptureHub&) { s.open("arranger"); }},
        {"outputSetting",
         [](ui::Shell& s, core::sampling::CaptureHub&)
         {
             s.home();
             s.open("mixer");
             s.open("mixer.output");
         }},
        {"samplingRecording",
         [](ui::Shell& s, core::sampling::CaptureHub& h)
         {
             s.home();
             s.open("track.menu");
             s.open("sampling.edit");
             h.status.takeScene = 0;
             h.status.takeTrack = 4;
             h.status.takeBars = 0; // REC LENGTH OFF: the editor doubles as the take grows
             h.status.takeStart = h.ring.head();
             h.status.takeEnd = -1;
             h.status.takeSeq = h.status.takeSeq.load() + 1;
             h.status.state = static_cast<int>(core::sampling::RecState::Recording);
         }},
        {"liveSampling",
         [](ui::Shell& s, core::sampling::CaptureHub& h)
         {
             h.status.state = static_cast<int>(core::sampling::RecState::Idle);
             s.back();
             s.open("sampling.live");
             h.status.liveState = static_cast<int>(core::sampling::LiveState::Recording);
             h.status.liveStart = h.ring.head();
             h.status.liveLength = 4 * 88200;
         }},
        {"midiNote",
         [](ui::Shell& s, core::sampling::CaptureHub&)
         {
             s.back();
             s.open("track.note");
         }},
        {"midiCc1",
         [](ui::Shell& s, core::sampling::CaptureHub&)
         {
             s.back();
             s.open("track.cc1");
         }},
        {"renderAudio",
         [](ui::Shell& s, core::sampling::CaptureHub&)
         {
             s.home();
             s.open("project");
             s.open("project.renderAudio");
         }},
    };
    double worst = 0;
    for (const auto& c : cases)
    {
        c.enter(shell, hub);
        for (int i = 0; i < 100; ++i)
            frame(i); // warm-up (and past the push transition)
        const auto s = measure(1000, frame);
        report(c.name, s, kScreenGateMs, dl.commands().size());
        worst = std::max(worst, s.p95);
        CHECK(s.p95 <= kScreenGateMs);
    }
    std::printf("BENCH ui_m5 worst screen p95 %.4f ms (A9 UI core budget 8 ms/frame)\n", worst);
    core::arrangement::liveSongStatus().row = -1;
    shell.home();
    session.reset();
}

TEST_CASE("bench_ui_m5: RingView scroll, PianoKeyboard, six CcCards, DataTable (9 of 999 rows)", "[bench][ui]")
{
    fakes::OffscreenRenderer offscreen;
    std::string error;
    REQUIRE(offscreen.open(800, 480, error));
    const auto& palette = ui::theme::palette(true);
    ui::FocusState focus;
    ui::UiContext ctx {palette, offscreen.measure(), 0, focus};
    ui::DrawList dl;
    dl.reserve(4096, 16384);

    // RingView: one 60 Hz frame of new input folded into the column cache, then the build (scrolling, recording).
    core::sampling::CaptureRing ring;
    REQUIRE(ring.allocate(44100 * 40));
    for (int i = 0; i < 1200; ++i) // 20 s of input first: the past 6 bars are full
        feedRing(ring, i);
    ui::RingView view;
    ui::LiveSamplingFeed feed;
    feed.ring = &ring;
    feed.playing = true;
    feed.state = static_cast<int>(core::sampling::LiveState::Recording);
    feed.takeStart = ring.head() - 88200;
    feed.takeLength = 4 * 88200;
    feed.bars = 4;
    feed.colour = 0xFFD9A100u;
    view.setFeed(feed);
    view.update();
    int n = 1200;
    const auto scroll = measure(1000,
                                [&](int)
                                {
                                    feedRing(ring, n++);
                                    feed.ppq += 0.0334;
                                    view.setFeed(feed);
                                    view.update();
                                    dl.clear();
                                    view.build(dl, ctx);
                                });
    report("ringView_scroll", scroll, 0.10, dl.commands().size());
    CHECK(scroll.p95 <= 0.10);

    ui::PianoKeyboardModel piano;
    piano.notes = {72, 79, 76, -1};
    piano.focusedNote = 72;
    piano.colour = 0xFF4FC3F7u;
    const auto keys = measure(1000,
                              [&](int i)
                              {
                                  piano.notes[0] = 60 + i % 24;
                                  dl.clear();
                                  ui::PianoKeyboard::draw(dl, ctx, piano);
                              });
    report("pianoKeyboard", keys, 0.02, dl.commands().size());
    CHECK(keys.p95 <= 0.02); // the whole keyboard built every frame (the damage diff repaints the changed keys)

    const auto cards = measure(1000,
                               [&](int i)
                               {
                                   dl.clear();
                                   for (int c = 0; c < 6; ++c)
                                   {
                                       ui::CcCardModel m;
                                       m.index = c;
                                       m.cc = 70 + c;
                                       ui::CcCard::nameOf(m.cc, m.name1, m.name2);
                                       m.active = ((i + c) & 1) != 0;
                                       m.focused = c == i % 6;
                                       ui::CcCard::draw(dl, ctx, m);
                                   }
                               });
    report("ccCards6", cards, 0.05, dl.commands().size());
    CHECK(cards.p95 <= 0.05);

    static constexpr ui::DataColumn cols[] = {{0, 80, "NO.", {}},        {80, 152, "POSITION", "BAR.STEP"},
                                              {232, 152, "LENGTH", "BAR.STEP"}, {384, 136, "SCENE", {}},
                                              {520, 136, "PATTERN", {}}, {656, 136, "REPEAT", {}},
                                              {792, 144, "BPM", {}}};
    ui::DataTableModel tm;
    tm.columns = cols;
    tm.rows = 999;
    tm.terminal = true;
    tm.terminalText = "END OF ARRANGEMENT";
    char text[16];
    const auto table = measure(1000,
                               [&](int i)
                               {
                                   tm.focused = (i * 7) % 1000;
                                   tm.first = ui::DataTable::scrollFor(tm.focused, tm.first, 1000, 9);
                                   tm.playing = (i / 3) % 999;
                                   dl.clear();
                                   ui::DataTable::drawFrame(dl, ctx, tm);
                                   for (int r = tm.first; r < std::min(1000, tm.first + 9); ++r)
                                   {
                                       const auto rr = ui::DataTable::drawRow(dl, ctx, tm, r);
                                       if (r == 999)
                                       {
                                           ui::DataTable::drawTerminal(dl, ctx, tm, rr);
                                           continue;
                                       }
                                       for (int c = 0; c < 7; ++c)
                                       {
                                           std::snprintf(text, sizeof text, "%03d", (r + c) % 1000);
                                           ui::DataTable::cell(dl, tm, palette, rr, c, text, 0, r == tm.focused);
                                       }
                                   }
                               });
    report("dataTable_9of999", table, 0.06, dl.commands().size());
    CHECK(table.p95 <= 0.06);
}
