// Send/Master Delay cycle budget: 9k A9 cycles per 64-frame stereo block, worst
// case "4000 ms, spread, both filters" — the longest time, spread 0.5 (both matrix paths), and hi-cut and lo-cut
// targets that flip every 64 blocks so both stepped filters always glide; a spread ramp every 256 blocks. The effect
// alone. Host timings are relative; the device number comes from scripts/emulator-bench.sh fx_delayBus / the hardware.
#include "tests/bench/harness/Bench.h"

#include "engine/EngineApi.h"
#include "engine/fx/effects/SendDelay.h"

#include <cmath>
#include <cstring>

namespace
{
void benchDelayBus(bench::State& state, bool modulate, bool tape)
{
    using namespace engine::fx;
    constexpr int N = engine::kBlockFrames;
    alignas(16) float l[N], r[N], inL[N], inR[N];
    for (int i = 0; i < N; ++i) // the stimulus, copied in every block (not timed as maths: two 256-byte copies)
    {
        inL[i] = 0.5f * std::sin(0.05f * static_cast<float>(i));
        inR[i] = 0.4f * std::cos(0.031f * static_cast<float>(i));
    }
    ProcessContext ctx{44100.0, 120.0, true, 0u, {}, 0};
    ctx.triggerOffset.fill(-1);
    SendDelay e;
    e.prepare(44100.0, N);
    const float k[6] = {1.0f, 0.8f, 0.3f, 0.4f, 0.5f, 1.0f};
    for (int i = 0; i < 6; ++i)
        e.setParam(i, k[i]);
    e.setOption(SendDelay::kOptMode, tape ? 1 : 0);
    e.reset();
    int iter = 0;
    state.run(
        [&]
        {
            ++iter;
            if (modulate)
                e.setParam(0, static_cast<float>(iter & 255) / 255.0f);
            if ((iter & 63) == 0)
            {
                e.setParam(2, (iter & 64) != 0 ? 0.8f : 0.3f);
                e.setParam(3, (iter & 64) != 0 ? 0.1f : 0.4f);
            }
            if ((iter & 255) == 0)
                e.setParam(4, (iter & 256) != 0 ? 0.6f : 0.5f);
            std::memcpy(l, inL, sizeof l);
            std::memcpy(r, inR, sizeof r);
            e.process(l, r, ctx);
            bench::doNotOptimize(l[3]);
        });
}
} // namespace

BENCH(fx_delayBus, 9000)
{
    benchDelayBus(state, false, false);
}

BENCH(fx_delayBus_digital_sweep, 9000)
{
    benchDelayBus(state, true, false);
}

BENCH(fx_delayBus_tape_sweep, 9000)
{
    benchDelayBus(state, true, true);
}
