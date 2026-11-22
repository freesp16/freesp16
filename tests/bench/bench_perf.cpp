// Ratchet / step-shift benchmarks (performance budget: scheduler ≤ 30 cycles per event,
// the worst case of 16 tracks × 16 repeats in one block ≤ 2k cycles, step shift ≤ 50 µs on the
// message thread for 64 steps × 35 locks). Budgets are A9 cycles at 1 GHz; host numbers are relative only (hardware
// numbers are measured on a unit).
//
//   bench_perf_repeats   audio thread, per block: 16 tracks each with one repeat due (count 16, interval exactly one
//                        block) emitted into a counting sink; re-armed every 15 blocks (arming amortised). 16 events:
//                        budget 16 × 30 = 480
//   bench_perf_worst     16 tracks armed with count 16 at a 4-frame interval: all 16 × 15 repeats in one block (the
//                        16 onStep() calls included), counting sink: budget 2000
//   bench_perf_stage     PerfStage::process on a block with 16 sequencer steps pending in a TriggerRouter (count 1:
//                        the scan, frame ordering and re-arm without repeats) + the router's own process() of those 16
//                        events (unattached: no sources; its cost is bench_trigger's): budget 2000 (our choice)
//   bench_perf_shift     message thread: CommandBus::execute of a SHIFT (ALL, 64 steps) on a track whose 64 cells hold
//                        35 locks each — apply, inverse, undo push (coalesced detents), change notification: 50 µs
#include "tests/bench/harness/Bench.h"

#include "core/commands/CommandBusAccess.h"
#include "core/commands/perf/ShiftSteps.h"
#include "core/model/ModelOps.h"
#include "core/model/Project.h"
#include "core/params/ParamRegistry.h"
#include "engine/sequencer/perf/PerfStage.h"
#include "engine/trigger/TriggerRouter.h"

#include <cstdio>
#include <cstdlib>
#include <memory>

namespace
{
namespace perf = engine::sequencer::perf;

struct Counter
{
    int n = 0;
    bool push(const engine::trigger::TriggerEvent& e) noexcept
    {
        n += e.velocity != 0 ? 1 : 0;
        return true;
    }
};
} // namespace

BENCH(bench_perf_repeats, 480)
{
    auto s = std::make_unique<perf::RetriggerScheduler>();
    Counter c;
    int block = 0;
    auto arm = [&] {
        for (int t = 0; t < 16; ++t)
            s->onStep(t, t, 100, {16, 0}, 512.0); // 1/8 step = 64 frames: one repeat per track per block
    };
    arm();
    state.run(
        [&]
        {
            if (++block == 15)
            {
                arm();
                block = 0;
            }
            s->emitBlock(c);
            bench::doNotOptimize(c.n);
        });
    if (c.n == 0)
        std::abort();
}

BENCH(bench_perf_worst, 2000)
{
    auto s = std::make_unique<perf::RetriggerScheduler>();
    Counter c;
    state.run(
        [&]
        {
            for (int t = 0; t < 16; ++t)
                s->onStep(t, 1, 100, {16, 0}, 32.0); // 1/8 of 32 frames: 15 repeats at 5, 9, … 61
            c.n = 0;
            s->emitBlock(c);
            bench::doNotOptimize(c.n);
        });
    if (c.n != 16 * 15)
    {
        std::fprintf(stderr, "bench_perf_worst: %d repeats in the block\n", c.n);
        std::abort();
    }
}

BENCH(bench_perf_stage, 2000)
{
    auto stage = std::make_unique<perf::PerfStage>();
    auto router = std::make_unique<engine::trigger::TriggerRouter>();
    router->prepare(44100.0, engine::kBlockFrames);
    stage->prepare(44100.0);
    engine::BlockContext ctx {};
    ctx.sampleRate = 44100.0;
    ctx.frames = engine::kBlockFrames;
    ctx.bpm = 120.0;
    ctx.playing = true;
    engine::stages::BlockBuffers::Timing timing {};
    engine::trigger::TriggerEvent e {};
    e.velocity = 100;
    e.kind = engine::trigger::TriggerKind::Full;
    e.source = engine::trigger::TriggerSource::Sequencer;
    state.run(
        [&]
        {
            for (int t = 0; t < 16; ++t)
            {
                e.track = static_cast<std::uint8_t>(t);
                e.frame = static_cast<std::uint8_t>(63 - 4 * t); // reverse order: the frame sort's worst case
                router->push(e);
            }
            stage->process(ctx, timing, *router);
            router->process(ctx);
            bench::doNotOptimize(router->blockEvents().data());
        });
}

BENCH(bench_perf_shift, 50000)
{
    core::CommandBus bus;
    core::commands::replaceProject(bus, core::model::newDefaultProject());
    auto& pat = core::commands::project(bus).scenes[0].patterns[0];
    pat.length = 64;
    for (int s = 0; s < 64; ++s)
    {
        auto& c = pat.steps[static_cast<std::size_t>(s)][3];
        c.trigger = 15;
        c.value2 = 100;
        core::model::StepLocks l;
        l.mask = (std::uint64_t {1} << 35) - 1;
        for (int k = 0; k < 35; ++k)
            l.values[static_cast<std::size_t>(k)] = static_cast<float>(k + s);
        pat.locks.push_back(l);
        c.lockIndex = static_cast<std::uint16_t>(pat.locks.size() - 1);
    }
    state.run(
        [&]
        {
            bus.execute(core::perf::shiftSteps(0, 0, 3, core::perf::ShiftArea::All, 0, 1));
            core::commands::flushNotifications(bus);
        });
}
