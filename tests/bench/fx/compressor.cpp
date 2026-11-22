// Compressor benches (budget: compressor 8k cycles per 64-frame stereo block on the
// Cortex-A9, 12k with RMS + look-ahead). One iteration = one block through Compressor::process (the effect
// alone; the shell's share is bench_fx_shell's). Desktop numbers are relative only: the A9 budget is confirmed on the
// device (scripts/emulator-bench.sh fx_compressor, the hardware).
//   fx_compressor                 peak detector, hard knee, compressing a signal whose level alternates every
//                                 4 blocks across the threshold (attack, hold, release, the per-block pow every block)
//   fx_compressor_rms_lookahead   RMS detector (double sum), soft knee, look-ahead toggled every 3 blocks so the
//                                 1/147 crossfade runs most of the time (the worst case of FX-1125)
#include "tests/bench/harness/Bench.h"

#include "engine/fx/effects/Compressor.h"

#include <cmath>
#include <cstring>
#include <memory>

namespace
{
using engine::fx::effects::Compressor;
constexpr int N = engine::kBlockFrames;
constexpr int kBlocks = 8;

struct Signal
{
    alignas(16) float l[kBlocks][N];
    alignas(16) float r[kBlocks][N];
    Signal()
    {
        for (int b = 0; b < kBlocks; ++b)
            for (int i = 0; i < N; ++i)
            {
                const float a = b < 4 ? 0.9f : 0.03f;
                const float t = static_cast<float>(b * N + i);
                l[b][i] = a * std::sin(0.071f * t);
                r[b][i] = a * std::cos(0.053f * t);
            }
    }
};

void setUp(Compressor& c, bool rms, bool lookAhead)
{
    c.prepare(44100.0, N);
    c.setParam(0, 22.0f / 52.0f); // −30 dB
    c.setParam(1, 70.0f / 290.0f); // 8:1
    c.setParam(2, 0.004f);
    c.setParam(3, 0.02f);
    c.setParam(4, 0.12f);
    c.setParam(5, 1.0f);
    c.setOption(Compressor::kOptDetect, rms ? 1 : 0);
    c.setOption(Compressor::kOptKnee, rms ? 1 : 0);
    c.setOption(Compressor::kOptLookAhead, lookAhead ? 1 : 0);
    c.reset();
}
} // namespace

BENCH(fx_compressor, 8000)
{
    auto sig = std::make_unique<Signal>();
    auto c = std::make_unique<Compressor>();
    setUp(*c, false, false);
    const engine::fx::ProcessContext ctx{44100.0, 120.0, true, 0u, {}, 0};
    alignas(16) float l[N], r[N];
    int b = 0;
    state.run(
        [&]
        {
            std::memcpy(l, sig->l[b], sizeof l);
            std::memcpy(r, sig->r[b], sizeof r);
            c->process(l, r, ctx);
            b = (b + 1) % kBlocks;
            bench::doNotOptimize(l[5]);
        });
}

BENCH(fx_compressor_rms_lookahead, 12000)
{
    auto sig = std::make_unique<Signal>();
    auto c = std::make_unique<Compressor>();
    setUp(*c, true, true);
    const engine::fx::ProcessContext ctx{44100.0, 120.0, true, 0u, {}, 0};
    alignas(16) float l[N], r[N];
    int b = 0, n = 0;
    state.run(
        [&]
        {
            if (++n % 3 == 0) // keep the look-ahead crossfade moving
                c->setOption(Compressor::kOptLookAhead, (n / 3) % 2);
            std::memcpy(l, sig->l[b], sizeof l);
            std::memcpy(r, sig->r[b], sizeof r);
            c->process(l, r, ctx);
            b = (b + 1) % kBlocks;
            bench::doNotOptimize(l[5]);
        });
}
