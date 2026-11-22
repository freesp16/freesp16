// Transport benchmarks (budget): transport advance + metronome click ≤ 4 µs = 4,000
// A9 cycles per 64-frame block. One iteration = one block of engine step 3 (TransportStage::process) plus the click
// (renderClick into BlockBuffers::aux), with a tempo change every block (a re-base each time: the worst case).
//   bench_transport        metronome on at 120–300 BPM, a SetTempo every block; also checks 1 h of such blocks against
//                          the closed form of the position (must be 0 within 1e-9 quarter) and prints the error
//   bench_transport_click  the click sounding in every block (Locate back onto a beat every 8 blocks at 300 BPM)
// Host numbers are ns at 1 GHz (harness convention); A9 numbers come from scripts/emulator-bench.sh (relative)
// and hardware (open).
#include "tests/bench/harness/Bench.h"

#include "engine/transport/TransportStage.h"

#if defined(BENCH_STANDALONE)
#include "ProjectIdentity.h" // the app's log tag: the emulator session keeps only tagged lines
#endif

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>

namespace
{
using engine::BlockContext;
using engine::TransportCmd;
namespace tr = engine::transport;

constexpr int kTempos = 8;
constexpr double kTempo[kTempos] = {120.0, 300.0, 87.3, 174.0, 133.7, 20.0, 99.9, 256.4};

struct Rig
{
    tr::TransportStage stage;
    engine::stages::BlockBuffers buffers {};
    BlockContext ctx {};
    std::uint64_t block = 0;

    void setUp(double rate)
    {
        stage.prepare(rate);
        stage.setParam(tr::TransportParam::Metronome, 1.0f);
        stage.command({TransportCmd::Play, 0});
        ctx.sampleRate = rate;
        ctx.frames = engine::kBlockFrames;
    }
    void runBlock(bool tempoChange) noexcept
    {
        if (tempoChange)
            stage.command({TransportCmd::SetTempo, kTempo[block % kTempos]});
        buffers.auxActive = false;
        stage.process(ctx, buffers);
        stage.renderClick(ctx, buffers);
        ++block;
    }
};

// 1 h at 44.1 kHz with a tempo change every block: |PPQ − Σ blocks · 64 · ppqPerFrame| (the closed form by counts).
double hourError(std::int64_t& blocksOut)
{
    tr::Transport t;
    t.prepare(44100.0);
    t.command({TransportCmd::Play, 0});
    const std::int64_t blocks = 3600LL * 44100 / 64;
    std::int64_t count[kTempos] = {};
    BlockContext c {};
    bool contiguous = true;
    double previous = 0.0;
    for (std::int64_t b = 0; b < blocks; ++b)
    {
        const int i = static_cast<int>(b % kTempos);
        t.command({TransportCmd::SetTempo, kTempo[i]});
        t.advance(c);
        contiguous = contiguous && c.ppqStart == previous;
        previous = c.ppqEnd;
        ++count[i];
    }
    double closed = 0.0;
    for (int i = 0; i < kTempos; ++i)
        closed += static_cast<double>(count[i] * 64) * kTempo[i] / (60.0 * 44100.0);
    blocksOut = blocks;
    return contiguous ? std::fabs(c.ppqEnd - closed) : 1.0;
}
} // namespace

BENCH(bench_transport, 4000)
{
    std::int64_t blocks = 0;
    const double error = hourError(blocks);
#if defined(BENCH_STANDALONE)
    std::printf("[sp16-mod] %s ", identity::envPrefix);
#endif
    std::printf("BENCH-CHECK bench_transport 1 h = %lld blocks, a tempo change every block: |ppq - closed form| = "
                "%.3g quarter (limit 1e-9) %s\n",
                static_cast<long long>(blocks), error, error <= 1e-9 ? "OK" : "FAIL");
    std::fflush(stdout);
#if ! defined(BENCH_STANDALONE)
    CHECK(error <= 1e-9);
#endif
    auto rig = std::make_unique<Rig>();
    rig->setUp(44100.0);
    state.run(
        [&]
        {
            rig->runBlock(true);
            bench::doNotOptimize(rig->buffers.aux.l[7]);
            bench::doNotOptimize(rig->ctx.ppqEnd);
        });
}

BENCH(bench_transport_click, 4000)
{
    auto rig = std::make_unique<Rig>();
    rig->setUp(44100.0);
    rig->stage.setParam(tr::TransportParam::Tempo, 300.0f);
    state.run(
        [&]
        {
            if (rig->block % 8 == 0) // back onto beat 0: a click restarts at frame 0 and sounds through 8 blocks
                rig->stage.command({TransportCmd::Locate, 0.0});
            rig->runBlock(false);
            bench::doNotOptimize(rig->buffers.aux.l[7]);
        });
}
