// Step-sequencer benchmark (budget: "sequencer collect, 16 tracks, locks, swing, record
// capture ≤ 15 µs per block on the A9" = 15 000 A9 cycles at 1 GHz. Host numbers are ns at 1 GHz (harness
// convention): they rank and guard regressions; A9 numbers come from scripts/emulator-bench.sh bench_sequencer
// (relative only) and hardware (open).
//
//   bench_sequencer        worst case per block: a 64-step pattern with a FULL trigger on every step of all 16 tracks,
//                          35 locks on every cell, offsets ±1 step (alternating −24 / +24 per step), swing 50 %,
//                          recording armed with 4 live hits captured per block (quantise 1/16T, "recorded this pass",
//                          EngineEvents), and each block placed on a step so all 16 tracks fire in it (12–16 events:
//                          the recorded hits suppress up to 4 of the next block's); plus the trigger
//                          router's sort of those 16 events (unattached: no track sources, their cost is bench_trigger's)
//   bench_sequencer_idle   the same pattern in a block between steps: the scan alone (the common case)
#include "tests/bench/harness/Bench.h"

#include "core/util/sequencer/PatternImage.h"
#include "engine/feeds/Feeds.h"
#include "engine/sequencer/PatternBuilder.h"
#include "engine/sequencer/Sequencer.h"
#include "engine/trigger/TriggerRouter.h"

#include <cstdio>
#include <cstdlib>
#include <memory>

namespace
{
using engine::sequencer::Sequencer;

struct Rig
{
    std::unique_ptr<engine::feeds::EngineFeeds> feeds = std::make_unique<engine::feeds::EngineFeeds>();
    std::unique_ptr<Sequencer> seq = std::make_unique<Sequencer>();
    std::unique_ptr<engine::trigger::TriggerRouter> router = std::make_unique<engine::trigger::TriggerRouter>();
    const engine::sequencer::PatternData* pattern = nullptr;
    engine::BlockContext ctx {};
    engine::stages::BlockBuffers::Timing timing {};
    double ppf = 0.0;
    int i = 0;

    Rig()
    {
        auto img = std::make_unique<core::sequencer::PatternImage>();
        img->length = 64;
        img->locks.resize(64 * 16);
        for (int s = 0; s < 64; ++s)
            for (int t = 0; t < 16; ++t)
            {
                auto& c = img->cells[static_cast<std::size_t>(s)][static_cast<std::size_t>(t)];
                c.velocity = 100;
                c.kind = 0;
                c.offset24 = static_cast<std::int8_t>((s & 1) ? 24 : -24);
                c.lockIndex = static_cast<std::uint16_t>(s * 16 + t);
                auto& l = img->locks[static_cast<std::size_t>(s * 16 + t)];
                l.mask = (std::uint64_t {1} << 35) - 1; // all 35 slots, offset (15) included
                for (int k = 0; k < 35; ++k)
                    l.values[static_cast<std::size_t>(k)] = static_cast<float>(k);
                l.values[15] = c.offset24;
            }
        pattern = engine::sequencer::buildPattern(*img);
        seq->prepare(44100.0);
        seq->setEventQueue(feeds.get());
        seq->install(0, 0, pattern);
        seq->setSwing(0.5f);
        seq->setRecord(true, core::sequencer::RecordQuantize::SixteenthTriplet, false);
        router->prepare(44100.0, engine::kBlockFrames);
        ctx.sampleRate = 44100.0;
        ctx.frames = engine::kBlockFrames;
        ctx.bpm = 120.0;
        ctx.playing = true;
        ppf = ctx.bpm / (60.0 * ctx.sampleRate);
    }
    ~Rig() { engine::sequencer::destroyPattern(pattern); }

    // A block on step s: [t − 1 frame, t + 63 frames) around the step's final time (grid + swing + offset).
    void place(int s, bool onStep) noexcept
    {
        const long pass = 16 + i / 64;
        const double grid = static_cast<double>(pass * 64 + s) * 0.25;
        const double fires = grid + ((s & 1) ? 0.125 + 0.25 : -0.25); // swing 50 % on odd steps, offset ±1 step
        ctx.ppqStart = onStep ? fires - ppf : grid + 0.1;               // between steps: no final time lands there
        ctx.ppqEnd = ctx.ppqStart + ppf * engine::kBlockFrames;
    }

    void block(bool onStep) noexcept
    {
        place(i & 63, onStep);
        seq->collect(ctx, timing, *router);
        if (onStep)
            for (int h = 0; h < 4; ++h)
            {
                engine::trigger::TriggerEvent e {};
                e.track = static_cast<std::uint8_t>((i + h * 5) & 15);
                e.velocity = 100;
                e.kind = engine::trigger::TriggerKind::Full;
                e.source = engine::trigger::TriggerSource::Pad;
                e.frame = static_cast<std::uint8_t>(h * 16);
                seq->onLiveHit(e, false);
            }
        router->process(ctx);
        ++i;
        if ((i & 255) == 0)
        {
            engine::EngineEvent ev; // the message thread drains the events once per frame
            while (feeds->popEvent(ev))
            {
            }
        }
    }
};

void run(bench::State& state, bool onStep)
{
    auto rig = std::make_unique<Rig>();
    for (int w = 0; w < 64; ++w)
    {
        rig->block(onStep);
        // The scenario is what the header says: 16 step events, less the ≤ 4 the previous block's recorded hits
        // suppress ("recorded this pass"); none between steps.
        const int n = rig->seq->eventsThisBlock();
        if (onStep ? (n < 12 || n > 16) : n != 0)
        {
            std::fprintf(stderr, "bench_sequencer: %d events in a block\n", n);
            std::abort();
        }
    }
    state.run(
        [&]
        {
            rig->block(onStep);
            bench::doNotOptimize(rig->router->blockEvents().data());
        });
}
} // namespace

BENCH(bench_sequencer, 15000) { run(state, true); }
BENCH(bench_sequencer_idle, 15000) { run(state, false); }
