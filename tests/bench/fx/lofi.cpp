// LoFi cycle budget: 12k A9 cycles per 64-frame stereo block, worst case
// "crusher + S&H + filter, Pre" — 9 bits, reduction 0.3, preamp 0.6, band-pass first, with a cutoff glide running
// every block (a coefficient recomputation every 44.1 samples). The effect alone (the shell is bench_fx_shell's).
// Host timings are relative; the device number comes from scripts/emulator-bench.sh fx_lofi / the hardware.
#include "tests/bench/harness/Bench.h"

#include "engine/EngineApi.h"
#include "engine/fx/effects/LoFi.h"

#include <cmath>

BENCH(fx_lofi, 12000)
{
    using namespace engine::fx;
    constexpr int N = engine::kBlockFrames;
    alignas(16) float l[N], r[N];
    for (int i = 0; i < N; ++i)
    {
        l[i] = 0.6f * std::sin(0.05f * static_cast<float>(i));
        r[i] = 0.5f * std::cos(0.031f * static_cast<float>(i));
    }
    ProcessContext ctx{44100.0, 120.0, true, 0u, {}, 0};
    ctx.triggerOffset.fill(-1);
    LoFi e;
    e.prepare(44100.0, N);
    const float k[6] = {0.6f, 0.3f, 0.5f, 0.5f, 0.4f, 1.0f};
    for (int i = 0; i < 6; ++i)
        e.setParam(i, k[i]);
    e.setOption(LoFi::kOptFilterType, LoFi::kBandPass);
    e.setOption(LoFi::kOptPosition, 0);
    e.reset();
    int iter = 0;
    state.run(
        [&]
        {
            if ((++iter & 7) == 0) // keep a 7.5-step cutoff glide running (below the 38-step gated jump)
                e.setParam(3, (iter & 8) != 0 ? 0.53f : 0.5f);
            e.process(l, r, ctx);
            bench::doNotOptimize(l[3]);
        });
}
