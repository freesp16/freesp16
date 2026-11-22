// Song mode benchmarks (Cortex-A9 performance budget): "≤ 200 cycles per block; O(1) row
// lookup; plan rebuild ≤ 0.5 ms for 999 rows on the message thread". One iteration = one 64-frame block (or one
// rebuild). The host is far faster than the A9: the gates are the A9 budgets, the numbers rank and guard regressions.
//
//   bench_arrangement        SectionSequencer::process + afterSwitch alone, per block, song playing (999 rows of
//                            8 steps at 300 BPM: a pass end every 138 blocks, one boundary computation every block).
//                            The switcher is not advanced (its advance is bench_scene's): the sequencer still finds
//                            every pass end, counts it and queues the next row, so its whole path is timed.
//   bench_arrangement_step5  the song with the scene switcher, as step 5 runs them (budget: bench_scene's 2 000 + 200)
//   bench_arrangement_plan   ArrangementPlanBuilder::buildPlan of 999 rows (message thread, budget 0.5 ms)
#include "tests/bench/harness/Bench.h"

#include "core/commands/arrangement/ArrangementPlanBuilder.h"
#include "core/model/ModelOps.h"
#include "core/util/Realtime.h"
#include "engine/scene/SectionSequencer.h"

#include <cstdint>
#include <memory>

namespace
{
struct Transport
{
    double tempo = 300.0;
    int commands = 0;
    double bpm() const noexcept { return tempo; }
    void command(const engine::TransportCmd& c) noexcept
    {
        ++commands;
        if (c.kind == engine::TransportCmd::SetTempo)
            tempo = c.value;
    }
};
struct Mixer
{
    void setScene(int, int) noexcept {}
    void setPatternMuteSolo(std::uint16_t, std::uint16_t, int) noexcept {}
};
struct Events
{
    int n = 0;
    bool pushEvent(const engine::EngineEvent&) noexcept { return ++n > 0; }
};

struct Rig
{
    std::unique_ptr<engine::scene::ArrangementPlan> plan = std::make_unique<engine::scene::ArrangementPlan>();
    engine::scene::ScenePatternSwitcher sw;
    engine::scene::SectionSequencer seq;
    engine::stages::BlockBuffers::Timing timing {};
    Transport transport;
    Mixer mixer;
    Events events;
    engine::BlockContext ctx {};
    std::int64_t block = 0;
    double ppqPerBlock = 0.0;

    Rig()
    {
        plan->count = 999;
        for (int i = 0; i < 999; ++i)
        {
            plan->rows[static_cast<std::size_t>(i)] = {static_cast<std::uint8_t>(i % 3 == 2), static_cast<std::uint8_t>(i % 2),
                                                       1, i % 5 == 0 ? 300.0f : 0.0f};
            plan->startPpq16[static_cast<std::size_t>(i)] = 8 * i;
        }
        plan->startPpq16[999] = 8 * 999;
        for (int s = 0; s < 2; ++s)
            for (int p = 0; p < 2; ++p)
                sw.setPatternLength(s, p, 8);
        seq.setSongMode(true);
        seq.setLoop(true); // never ends while timed
        ppqPerBlock = 300.0 / (60.0 * 44100.0) * engine::kBlockFrames;
        ctx.sampleRate = 44100.0;
        ctx.frames = engine::kBlockFrames;
        ctx.bpm = 300.0;
        // Installed and started outside the timed loop.
        ctx.playing = false;
        seq.process(ctx, timing, sw, transport, plan.get());
        seq.afterSwitch(ctx, timing, transport, transport.bpm());
        ctx.playing = true;
    }
    void next() noexcept
    {
        ctx.startedThisBlock = block == 0;
        ctx.ppqStart = static_cast<double>(block) * ppqPerBlock;
        ctx.ppqEnd = static_cast<double>(block + 1) * ppqPerBlock;
        ctx.blockIndex = static_cast<std::uint64_t>(block);
        ++block;
    }
};
} // namespace

BENCH(bench_arrangement, 200)
{
    Rig rig;
    core::rt::ScopedRealtimeThread rt; // no allocation in the timed loop (aborts under APP_RT_CHECKS)
    state.run(
        [&]
        {
            rig.next();
            rig.seq.process(rig.ctx, rig.timing, rig.sw, rig.transport, rig.plan.get());
            rig.seq.afterSwitch(rig.ctx, rig.timing, rig.transport, rig.transport.bpm());
            bench::doNotOptimize(rig.seq.songPosition16th());
        });
#if ! defined(BENCH_STANDALONE)
    CHECK(rig.seq.active());
    CHECK(rig.seq.rowChanges() > 1);
#endif
}

BENCH(bench_arrangement_step5, 2200)
{
    Rig rig;
    core::rt::ScopedRealtimeThread rt;
    state.run(
        [&]
        {
            rig.next();
            rig.seq.process(rig.ctx, rig.timing, rig.sw, rig.transport, rig.plan.get());
            const double before = rig.transport.bpm();
            const auto r = rig.sw.process(rig.ctx, rig.timing, rig.transport, rig.mixer, rig.events);
            rig.seq.afterSwitch(rig.ctx, rig.timing, rig.transport, before);
            bench::doNotOptimize(r);
        });
#if ! defined(BENCH_STANDALONE)
    CHECK(rig.events.n > 1); // the rows switched
#endif
}

BENCH(bench_arrangement_plan, 500000)
{
    auto project = core::model::newDefaultProject();
    project->arrangement.sections.resize(999);
    for (int i = 0; i < 999; ++i)
    {
        auto& s = project->arrangement.sections[static_cast<std::size_t>(i)];
        s.scene = static_cast<std::uint8_t>(i % 16);
        s.pattern = static_cast<std::uint8_t>((i * 7) % 16);
        s.repeats = 1 + i % 99;
        if (i % 4 == 0)
            s.fixedBpm = 90.0 + i % 100;
    }
    auto plan = std::make_unique<core::arrangement::ArrangementPlan>();
    state.run(
        [&]
        {
            core::commands::arrangement::buildPlan(*project, *plan);
            bench::doNotOptimize(plan->startPpq16[999]);
        });
#if ! defined(BENCH_STANDALONE)
    CHECK(plan->count == 999);
#endif
}
