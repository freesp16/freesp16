// MIDI engine benchmarks (performance budget): engine MIDI ≤ 3k cycles per 64-frame block
// typical (≤ 64 events in, ≤ 64 out), ≤ 8k worst case (a full CC burst). One iteration = one block of the MIDI stage:
// beginPeriod + processIn (drain the hub, schedule, apply) + the router's sort (sources detached: the MIDI hits only)
// + processOut (8 MIDI tracks with 2-note chords, clock out, hanging-note bookkeeping) + the sender-side drain.
//   bench_midi        32 notes + 32 CC 7 in, 8 chord triggers (16 notes + their offs) and the clock out
//   bench_midi_burst  128 CCs in one block (CC 7 / 10 / 17 / 46 on 16 channels), the FX position path included
// Host numbers are ns at 1 GHz (harness convention); A9 numbers come from scripts/emulator-bench.sh and hardware.
#include "tests/bench/harness/Bench.h"

#include "core/params/ParamRegistry.h"
#include "core/util/midi/CcTable.h"
#include "engine/midi/MidiEngine.h"
#include "engine/scene/ScenePatternSwitcher.h"
#include "engine/stages/BlockBuffers.h"
#include "engine/transport/TransportStage.h"
#include "engine/trigger/TriggerRouter.h"
#include "io/midi/CcTableBuilder.h"

#include <cstdint>
#include <memory>

namespace
{
struct Sink final : engine::midi::ParamSink, engine::midi::FxCcContext
{
    std::uint64_t applied = 0;
    void applyParam(const engine::ParamChange& c) noexcept override { applied += c.param.value; }
    int insertType(int, int) const noexcept override { return 0; }
    bool insertSync(int, int, int) const noexcept override { return false; }
};

struct Rig
{
    engine::trigger::TriggerRouter router;
    engine::transport::TransportStage transport;
    engine::scene::ScenePatternSwitcher switcher;
    engine::midi::MidiEngine midi;
    engine::stages::BlockBuffers buffers {};
    engine::BlockContext ctx {};
    core::midi::CcTable table {};
    Sink sink;
    std::int64_t block = 0;

    void setUp()
    {
        engine::bridge::ParamBindings bindings;
        const auto resolve = [](std::string_view id) {
            return core::builtinParams().idOf(id).value_or(core::ParamId {});
        };
        midi.bindParams(resolve, bindings);
        midi.attach({&router, &transport, &switcher, nullptr, nullptr, nullptr, &sink, &sink, nullptr, nullptr});
        transport.prepare(44100.0);
        midi.prepare(44100.0);
        io::midi::buildCcTable(core::builtinParams(), table);
        midi.hub().setCcTable(&table);
        midi.hub().setOutputConnected(0, true);
        for (int t = 0; t < 8; ++t) // tracks 1–8 MIDI, 2-note chords
        {
            router.setTrackParam(0, t, engine::trigger::TrackField::Mode, 1.0f);
            midi.onParam({resolve("midi.note2"), static_cast<std::uint16_t>(t), 67.0f, 0});
        }
        midi.onParam({resolve("midi.out.clock"), 0, 1.0f, 0});
        midi.onParam({resolve("midi.out.startStop"), 0, 1.0f, 0});
        transport.command({engine::TransportCmd::Play, 0.0});
        ctx.sampleRate = 44100.0;
        ctx.frames = engine::kBlockFrames;
    }
    void push(std::uint8_t b0, std::uint8_t b1, std::uint8_t b2, int i) noexcept
    {
        core::midi::MidiEvent e;
        e.port = 0;
        e.size = 3;
        e.bytes[0] = b0;
        e.bytes[1] = b1;
        e.bytes[2] = b2;
        // Arrived during the previous block: due in this one at frame i (latency = one block).
        e.hostTimeNs = static_cast<std::int64_t>((static_cast<double>(block * 64 - 64 + (i & 63))) * 1.0e9 / 44100.0);
        midi.hub().pushIn(0, e);
    }
    void runBlock(int notes, int ccs, int chords) noexcept
    {
        ctx.blockIndex = static_cast<std::uint64_t>(block);
        midi.beginPeriod(block * 64, 0, 64);
        for (int i = 0; i < notes; ++i)
            push(static_cast<std::uint8_t>(0x90 | (8 + (i & 7))), static_cast<std::uint8_t>(8 + (i & 7)), 100, i);
        static constexpr std::uint8_t kCcs[] = {7, 10, 17, 46};
        for (int i = 0; i < ccs; ++i)
            push(static_cast<std::uint8_t>(0xB0 | (i & 15)), kCcs[(i >> 4) & 3], static_cast<std::uint8_t>(i & 127), i);
        midi.processIn(ctx);
        transport.process(ctx, buffers);
        if (block % 4 == 0)
            for (int t = 0; t < chords; ++t)
            {
                engine::trigger::TriggerEvent e {};
                e.track = static_cast<std::uint8_t>(t);
                e.velocity = 90;
                e.kind = engine::trigger::TriggerKind::Full;
                e.source = engine::trigger::TriggerSource::Sequencer;
                e.frame = static_cast<std::uint8_t>(t * 7);
                router.push(e);
            }
        router.process(ctx);
        midi.processOut(ctx, buffers);
        core::midi::MidiOutEvent o;
        while (midi.hub().popOut(o))
            bench::doNotOptimize(o.sampleTime);
        ++block;
    }
};
} // namespace

BENCH(bench_midi, 3000)
{
    auto rig = std::make_unique<Rig>();
    rig->setUp();
    state.run([&] {
        rig->runBlock(32, 32, 8);
        bench::doNotOptimize(rig->sink.applied);
    });
}

BENCH(bench_midi_burst, 8000)
{
    auto rig = std::make_unique<Rig>();
    rig->setUp();
    state.run([&] {
        rig->runBlock(0, 128, 8);
        bench::doNotOptimize(rig->sink.applied);
    });
}
