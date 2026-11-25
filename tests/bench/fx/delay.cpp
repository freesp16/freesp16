// Insert Delay cycle budget: 6k A9 cycles per 64-frame stereo block, worst case
// "1000 ms, hi-cut glide" — the longest time (the ring walk at its widest), feedback 0.9, and a hi-cut target that
// flips every 64 blocks so the stepped filter is always gliding (a coefficient recomputation, tan(), every 1/3 ms).
// fx_delay_steady: the same without the glide. The effect alone (the shell is bench_fx_shell's). Host timings are
// relative; the device number comes from scripts/emulator-bench.sh fx_delay / the hardware.
#include "tests/bench/harness/Bench.h"

#include "engine/EngineApi.h"
#include "engine/fx/effects/InsertDelay.h"

#include <cmath>
#include <cstring>

namespace
{
void benchDelay(bench::State& state, bool glide, bool modulate = false, bool tape = false)
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
    InsertDelay e;
    e.prepare(44100.0, N);
    const float k[6] = {1.0f, 0.9f, 0.3f, 0.0f, 0.0f, 1.0f};
    for (int i = 0; i < 6; ++i)
        e.setParam(i, k[i]);
    e.setOption(InsertDelay::kOptMode, tape ? 1 : 0);
    e.reset();
    int iter = 0;
    state.run(
        [&]
        {
            ++iter;
            if (modulate)
                e.setParam(0, static_cast<float>(iter & 255) / 255.0f);
            if (glide && (iter & 63) == 0) // keep the hi-cut gliding (≈ 500 steps between the two targets)
                e.setParam(2, (iter & 64) != 0 ? 0.8f : 0.3f);
            std::memcpy(l, inL, sizeof l);
            std::memcpy(r, inR, sizeof r);
            e.process(l, r, ctx);
            bench::doNotOptimize(l[3]);
        });
}
} // namespace

BENCH(fx_delay, 6000)
{
    benchDelay(state, true);
}

BENCH(fx_delay_steady, 6000) // no glide: the common case
{
    benchDelay(state, false);
}

BENCH(fx_delay_digital_sweep, 6000)
{
    benchDelay(state, true, true, false);
}

BENCH(fx_delay_tape_sweep, 6000)
{
    benchDelay(state, true, true, true);
}
