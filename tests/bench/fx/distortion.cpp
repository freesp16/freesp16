// Distortion cycle budget (FX-051: the dearest insert): 14k A9 cycles per 64-frame stereo block,
// worst case auto-gain on (Dynamics 0.5 with the input crossing the threshold, so every branch runs), full drive
// (40 dB), lo/hi freq mid-range. The effect alone (the shell is bench_fx_shell's). Host timings are relative.
#include "tests/bench/harness/Bench.h"

#include "engine/EngineApi.h"
#include "engine/fx/effects/Distortion.h"

#include <cmath>

BENCH(fx_distortion, 14000)
{
    using namespace engine::fx;
    constexpr int N = engine::kBlockFrames;
    alignas(16) float l[N], r[N], srcL[N], srcR[N];
    for (int i = 0; i < N; ++i)
    {
        srcL[i] = 0.6f * std::sin(0.05f * static_cast<float>(i)) * (i < 32 ? 1.0f : 0.01f);
        srcR[i] = 0.4f * std::cos(0.031f * static_cast<float>(i)) * (i < 32 ? 0.02f : 1.0f);
    }
    ProcessContext ctx{44100.0, 120.0, true, 0u, {}, 0};
    ctx.triggerOffset.fill(-1);
    Distortion e;
    e.prepare(44100.0, N);
    const float k[6] = {1.0f, 0.8f, 0.3f, 0.7f, 0.5f, 1.0f};
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
