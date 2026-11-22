// Chorus cycle budget: 10k A9 cycles per 64-frame stereo block, worst case
// "441-sample sweep, sync" — width 100, delay ×10 (0.2…441.2 samples), SYNC 1/16 beat at 120 BPM (32 Hz), 180°, in
// the Reference profile (its output-gain stage runs every block; the Sixteen profile skips it).
// The effect alone (the shell is bench_fx_shell's). Host timings are relative; the device number comes from
// scripts/emulator-bench.sh fx_chorus / the hardware.
#include "tests/bench/harness/Bench.h"

#include "engine/EngineApi.h"
#include "engine/fx/FxShell.h"
#include "engine/fx/effects/Chorus.h"

#include <cmath>

BENCH(fx_chorus, 10000)
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
    Chorus e;
    e.prepare(44100.0, N);
    const float k[6] = {1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 1.0f};
    for (int i = 0; i < 6; ++i)
        e.setParam(i, k[i]);
    e.setOption(Chorus::kOptSync, 1);
    e.setQuirkProfile(kQuirkProfileReference);
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
