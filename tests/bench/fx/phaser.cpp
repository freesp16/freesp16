// Phaser cycle budget: 14k A9 cycles per 64-frame stereo block, worst case
// "6 all-pass stages, feedback" — feedback 0.99, width 1, 18 kHz, 10 Hz, 180°, with a Frequency change every 8th
// block (corner tangents and the frequency law recomputed, the 192-sample ramp running). The effect alone (the shell
// is bench_fx_shell's). Host timings are relative; the device number comes from scripts/emulator-bench.sh fx_phaser /
// the hardware.
#include "tests/bench/harness/Bench.h"

#include "engine/EngineApi.h"
#include "engine/fx/effects/Phaser.h"

#include <cmath>

BENCH(fx_phaser, 14000)
{
    using namespace engine::fx;
    constexpr int N = engine::kBlockFrames;
    alignas(16) float l[N], r[N], srcL[N], srcR[N];
    for (int i = 0; i < N; ++i)
    {
        srcL[i] = 0.6f * std::sin(0.05f * static_cast<float>(i));
        srcR[i] = 0.5f * std::cos(0.031f * static_cast<float>(i));
    }
    ProcessContext ctx{44100.0, 120.0, true, 0u, {}, 0};
    ctx.triggerOffset.fill(-1);
    Phaser e;
    e.prepare(44100.0, N);
    const float k[6] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    for (int i = 0; i < 6; ++i)
        e.setParam(i, k[i]);
    e.reset();
    int iter = 0;
    state.run(
        [&]
        {
            if ((++iter & 7) == 0)
                e.setParam(3, (iter & 8) != 0 ? 0.9f : 1.0f);
            for (int i = 0; i < N; ++i)
            {
                l[i] = srcL[i];
                r[i] = srcR[i];
            }
            e.process(l, r, ctx);
            bench::doNotOptimize(l[3]);
        });
}
