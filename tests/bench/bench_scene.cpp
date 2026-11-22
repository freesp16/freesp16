// Scene/pattern switcher benchmarks (budget: "scene/pattern switcher ≤ 2 µs per block on
// the A9" (2 000 cycles at 1 GHz). One iteration = one 64-frame block of step 5 (advance + the side effects into the real
// engine::mixer::Mixer, a counting transport and event sink). The host is far faster than the A9: the gate is the A9
// budget, the numbers rank and guard regressions; device-relative numbers come from scripts/emulator-bench.sh
// bench_scene, hardware numbers are open.
//
//   bench_scene          the budget scenario: 120 BPM, a switch requested every bar with the STEP unit, cycling through
//                        the 16 scenes (scene change → Mixer::setScene + pattern masks every bar); runs for as many
//                        blocks as the harness asks (≈ 1 h of blocks over the calibration and samples), on a thread
//                        marked real-time (APP_RT_CHECKS builds abort on any allocation; the Debug soak test
//                        scene.soak covers exactly 1 h of blocks)
//   bench_scene_pending  worst case per block: a request always pending at 300 BPM with the BAR unit (the boundary is
//                        computed every block), scene BPM applied on every switch (SetTempo), CONTINUE mode
#include "tests/bench/harness/Bench.h"

#include "core/util/Realtime.h"
#include "engine/mixer/Mixer.h"
#include "engine/scene/ScenePatternSwitcher.h"

#include <cstdint>
#include <memory>

namespace
{
struct CountingTransport
{
    int tempos = 0;
    void command(const engine::TransportCmd&) noexcept { ++tempos; }
};
struct CountingEvents
{
    int events = 0;
    bool pushEvent(const engine::EngineEvent&) noexcept { return ++events > 0; }
};

// The transport's drift-free block PPQ: p = blocks × 64 × ppqPerFrame.
struct Rig
{
    std::unique_ptr<engine::mixer::Mixer> mixer = std::make_unique<engine::mixer::Mixer>();
    engine::scene::ScenePatternSwitcher sw;
    engine::stages::BlockBuffers::Timing timing {};
    CountingTransport transport;
    CountingEvents events;
    engine::BlockContext ctx {};
    std::int64_t block = 0;
    double ppqPerBlock = 0.0, nextRequest = 0.0, requestEvery = 4.0;
    int requested = 0;

    explicit Rig(double bpm)
    {
        mixer->prepare(44100.0, engine::kBlockFrames);
        ppqPerBlock = bpm / (60.0 * 44100.0) * engine::kBlockFrames;
        ctx.sampleRate = 44100.0;
        ctx.frames = engine::kBlockFrames;
        ctx.bpm = bpm;
        ctx.playing = true;
        for (int s = 0; s < 16; ++s)
            for (int p = 0; p < 16; ++p)
            {
                sw.setPatternLength(s, p, 16 + ((s + p) & 3) * 8);
                sw.setPatternMuteSolo(s, p, static_cast<std::uint16_t>(1u << p), static_cast<std::uint16_t>(s == p));
            }
    }
    void block1() noexcept
    {
        ctx.startedThisBlock = block == 0;
        ctx.ppqStart = static_cast<double>(block) * ppqPerBlock;
        ctx.ppqEnd = static_cast<double>(block + 1) * ppqPerBlock;
        ctx.blockIndex = static_cast<std::uint64_t>(block);
        if (ctx.ppqStart >= nextRequest)
        {
            const int target = (requested % 16) * 16 + 1 + (requested & 1) * 2;
            sw.request(target / 16, target % 16);
            ++requested;
            nextRequest += requestEvery;
        }
        const auto r = sw.process(ctx, timing, transport, *mixer, events);
        bench::doNotOptimize(r);
        bench::doNotOptimize(timing.patternStartPpq);
        ++block;
    }
};
} // namespace

BENCH(bench_scene, 2000)
{
    Rig rig(120.0);
    rig.sw.setUnit(engine::scene::SwitchUnit::Step);
    core::rt::ScopedRealtimeThread rt; // no allocation in the timed loop (aborts under APP_RT_CHECKS)
    state.run([&] { rig.block1(); });
#if ! defined(BENCH_STANDALONE)
    CHECK(rig.events.events >= rig.requested - 1); // every bar's request switched
    CHECK(rig.block > 0);
#endif
}

BENCH(bench_scene_pending, 2000)
{
    Rig rig(300.0);
    rig.sw.setUnit(engine::scene::SwitchUnit::Bar);
    rig.sw.setContinue(true);
    for (int s = 0; s < 16; ++s)
        rig.sw.setSceneBpm(s, 300.0f);
    rig.requestEvery = 0.0; // a new request every block: one is always pending
    core::rt::ScopedRealtimeThread rt;
    state.run([&] { rig.block1(); });
#if ! defined(BENCH_STANDALONE)
    CHECK(rig.transport.tempos > 0);
#endif
}
