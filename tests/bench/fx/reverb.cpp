// Reverb cycle budget: 60k A9 cycles per 64-frame stereo block, worst case
// "5 stereo combs + 2 all-passes, room glide" — the room target flips between 20 and 80 every 128 blocks so the combs
// keep gliding (integer reads at moving positions, chunks split at room steps, 10 pow() per step), spread 0.5 and a
// mid damping; the Sixteen profile (ungated). The effect alone. Host timings are relative; the device number comes
// from scripts/emulator-bench.sh fx_reverb / the hardware. Steady state (no glide) is reported as fx_reverb_steady.
#include "tests/bench/harness/Bench.h"

#include "engine/EngineApi.h"
#include "engine/fx/effects/Reverb.h"

#include <cmath>
#include <cstring>

namespace
{
void benchReverb(bench::State& state, bool glide)
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
    Reverb e;
    e.prepare(44100.0, N);
    const float k[6] = {0.3f, 0.2f, 0.6f, 0.4f, 0.5f, 1.0f};
    for (int i = 0; i < 6; ++i)
        e.setParam(i, k[i]);
    e.reset();
    int iter = 0;
    state.run(
        [&]
        {
            ++iter;
            if (glide && (iter & 127) == 0)
                e.setParam(1, (iter & 128) != 0 ? 0.8f : 0.2f);
            std::memcpy(l, inL, sizeof l);
            std::memcpy(r, inR, sizeof r);
            e.process(l, r, ctx);
            bench::doNotOptimize(l[3]);
        });
}
} // namespace

BENCH(fx_reverb, 60000)
{
    benchReverb(state, true);
}

BENCH(fx_reverb_steady, 60000)
{
    benchReverb(state, false);
}
