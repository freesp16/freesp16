// Vintage Pitch Shifter cycle budget: 40k A9 cycles per 64-frame stereo block (2.8 % of the A9 at 44.1 kHz; the
// reverb's is 60k). Most of it is latency-bound recursion (compander detectors, the 4-pole AA filter, the
// reconstruction filter) and the sequential clock events, so it is set from the recursive effects' host/budget ratio
// (phaser 8 %, compressor 6 %), not the feed-forward ones'; to be confirmed on the device. Worst case: PITCH +12 st
// (the read clock at 2 × FREF, so ~97 read events and ~49 write events per block at 44.1 kHz), FEEDBACK +100 % (the
// loop active), AGE 100 % (every converter error, dead LSBs, VCO wobble), DELAY 120 ms, the input loud enough for the
// A/D to clip now and then (LIMIT). The effect alone (the shell is bench_fx_shell's). Host timings are relative.
#include "tests/bench/harness/Bench.h"

#include "engine/EngineApi.h"
#include "engine/fx/effects/VintagePitch.h"

#include <cmath>

BENCH(fx_vpitch, 40000)
{
    using namespace engine::fx;
    constexpr int N = engine::kBlockFrames;
    alignas(16) float l[N], r[N], srcL[N], srcR[N];
    for (int i = 0; i < N; ++i)
    {
        srcL[i] = 0.9f * std::sin(0.05f * static_cast<float>(i)) * (i < 32 ? 1.0f : 0.2f);
        srcR[i] = 0.7f * std::cos(0.031f * static_cast<float>(i)) * (i < 32 ? 0.2f : 1.0f);
    }
    ProcessContext ctx{44100.0, 120.0, true, 0u, {}, 0};
    ctx.triggerOffset.fill(-1);
    static VintagePitch e; // ~55 KB of RAM words and tables: not on the bench thread's stack
    e.prepare(44100.0, N);
    const float k[6] = {1.0f, 0.5f, 1.0f, 1.0f, 1.0f, 1.0f};
    for (int i = 0; i < 6; ++i)
        e.setParam(i, k[i]);
    e.reset();
    state.run(
        [&]
        {
            for (int i = 0; i < N; ++i)
            {
                l[i] = srcL[i];
                r[i] = srcR[i];
            }
            e.process(l, r, ctx);
            bench::doNotOptimize(l[3]);
        });
}
