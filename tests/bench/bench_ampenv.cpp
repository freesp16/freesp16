// Amp-envelope benchmarks (Cortex-A9 performance budget): 16 envelopes ≤ 10 µs = 10 k A9 cycles per 64-frame
// block, worst case "all in attack/release segments", with coefficient recomputation (setParams) on all 16 tracks
// every block (automation everywhere). One iteration = one block of the 16-lane bank, gains applied to 16 stereo
// track buffers.
//   bench_ampenv        fixed tempo: every track in a LINEAR or CURVE attack or release, retriggered and released in
//                       staggered blocks (so events land inside blocks and split them), setParams × 16 per block
//   bench_ampenv_tempo  the same with SYNC on and the tempo changing every block (16 sync steps recomputed, 32 exp)
// Host numbers are ns at 1 GHz (harness convention); A9 numbers come from scripts/emulator-bench.sh (relative)
// and hardware (open).
#include "tests/bench/harness/Bench.h"

#include "engine/modulation/AmpEnvelopeBank.h"

#include <array>
#include <memory>

namespace
{
using namespace engine::modulation;
constexpr int kTracks = engine::kTracks, kFrames = engine::kBlockFrames;

struct Rig
{
    AmpEnvelopeBank bank;
    std::array<AmpEnvParams, kTracks> params {};
    alignas(16) std::array<std::array<float, kFrames>, kTracks> l {}, r {};
    std::uint64_t block = 0;

    void setUp(bool sync)
    {
        bank.prepare(44100.0);
        for (int t = 0; t < kTracks; ++t)
        {
            auto& p = params[static_cast<std::size_t>(t)];
            p.attack = static_cast<std::uint8_t>(90 + t); // ≈ 0.6–1.5 s: always ramping between retriggers
            p.hold = static_cast<std::uint8_t>(95 + t);   // finite: the release follows on its own too
            p.release = static_cast<std::uint8_t>(100 + t);
            p.attackCurve = (t & 1) ? EnvCurve::Curve : EnvCurve::Linear;
            p.releaseCurve = (t & 2) ? EnvCurve::Curve : EnvCurve::Linear;
            p.sync = sync;
            bank.setParams(t, p, 120.0);
            for (int i = 0; i < kFrames; ++i)
                l[static_cast<std::size_t>(t)][static_cast<std::size_t>(i)] =
                    r[static_cast<std::size_t>(t)][static_cast<std::size_t>(i)] = 0.25f;
        }
    }

    void runBlock(bool tempoRamp) noexcept
    {
        const double bpm = tempoRamp ? 100.0 + static_cast<double>(block % 64) * 0.5 : 120.0;
        for (int t = 0; t < kTracks; ++t)
        {
            auto& p = params[static_cast<std::size_t>(t)];
            p.velocity = static_cast<std::uint8_t>(100 + ((block + static_cast<std::uint64_t>(t)) & 15));
            bank.setParams(t, p, bpm); // automation on every track, every block
            const auto phase = (block + static_cast<std::uint64_t>(t) * 37) % 160;
            if (phase == 0)
            {
                engine::trigger::TriggerEvent e {};
                e.track = static_cast<std::uint8_t>(t);
                e.velocity = 100;
                e.frame = static_cast<std::uint8_t>((t * 5) & 63);
                bank.trigger(e, true);
            }
            else if (phase == 60)
                bank.release(t, (t * 7) & 63); // gate up: attack/hold → release
            bank.process(t, l[static_cast<std::size_t>(t)].data(), r[static_cast<std::size_t>(t)].data());
        }
        ++block;
    }
};

void warmUp(Rig& rig, bool tempo)
{
    for (int i = 0; i < 400; ++i) // every track triggered at least twice: mixed stages
        rig.runBlock(tempo);
}
} // namespace

BENCH(bench_ampenv, 10000)
{
    auto rig = std::make_unique<Rig>();
    rig->setUp(false);
    warmUp(*rig, false);
    state.run(
        [&]
        {
            rig->runBlock(false);
            bench::doNotOptimize(rig->l[3][17]);
        });
}

BENCH(bench_ampenv_tempo, 10000)
{
    auto rig = std::make_unique<Rig>();
    rig->setUp(true);
    warmUp(*rig, true);
    state.run(
        [&]
        {
            rig->runBlock(true);
            bench::doNotOptimize(rig->l[3][17]);
        });
}
