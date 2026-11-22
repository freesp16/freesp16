// Shared modulation oscillator kernel budget: 1.5k A9 cycles per 64-frame stereo block for the
// worst case of its users — the stereo sine (double recursion) with the 50 Hz smoother on, as Chorus/Flanger run it.
// Host timings are relative.
#include "tests/bench/harness/Bench.h"

#include "engine/EngineApi.h"
#include "engine/fx/effects/Oscillator.h"

BENCH(fx_oscillator, 1500)
{
    using namespace engine::fx;
    constexpr int N = engine::kBlockFrames;
    alignas(16) float l[N], r[N];
    ModOscillator o;
    o.init(ModOscillator::kSine, 2.3f, 44100.0);
    o.setAmplitude(22.05f);
    o.setOffset(22.25f);
    o.setPhaseOffset(0.25f);
    state.run(
        [&]
        {
            o.process(l, r, N);
            bench::doNotOptimize(l[3]);
        });
}
