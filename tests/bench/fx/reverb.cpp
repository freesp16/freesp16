// Reverb cycle budget: 14k A9 cycles per 64-frame stereo block (the dearest insert's, Distortion, FX-051), so a
// Reverb on each of the 16 tracks fits the insert budget the MASTER TEMPO governor assumes.
//   fx_reverb          worst case: the room flips between 20 and 80 every 128 blocks, so every line keeps gliding
//                      (eight interpolated reads instead of four) and the coefficients are recomputed (libm)
//   fx_reverb_steady   steady state: four plain block copies, four modulated reads
//   fx_reverb_16       16 track reverbs, all running (budget 16 × 14k)
//   fx_reverb_16_idle  16 track reverbs on silent tracks, asleep after their tails: a peak scan each
// Host timings are relative; the device number comes from scripts/emulator-bench.sh fx_reverb / the hardware.
#include "tests/bench/harness/Bench.h"

#include "engine/EngineApi.h"
#include "engine/fx/effects/Reverb.h"

#include <cmath>
#include <cstring>
#include <memory>

namespace
{
using namespace engine::fx;
constexpr int N = engine::kBlockFrames;

struct Stimulus
{
    alignas(16) float l[N], r[N], inL[N], inR[N];
    Stimulus()
    {
        for (int i = 0; i < N; ++i) // copied in every block (not timed as maths: two 256-byte copies)
        {
            inL[i] = 0.5f * std::sin(0.05f * static_cast<float>(i));
            inR[i] = 0.4f * std::cos(0.031f * static_cast<float>(i));
        }
    }
};

void setUp(Reverb& e, float time = 0.6f) // 0.6: T60 14.5 s; 0.3: 3.55 s
{
    e.prepare(44100.0, N);
    const float k[6] = {0.3f, 0.2f, time, 0.4f, 0.5f, 1.0f};
    for (int i = 0; i < 6; ++i)
        e.setParam(i, k[i]);
    e.reset();
}

void benchReverb(bench::State& state, bool glide)
{
    Stimulus s;
    ProcessContext ctx{44100.0, 120.0, true, 0u, {}, 0};
    ctx.triggerOffset.fill(-1);
    Reverb e;
    setUp(e);
    int iter = 0;
    state.run(
        [&]
        {
            ++iter;
            if (glide && (iter & 127) == 0)
                e.setParam(1, (iter & 128) != 0 ? 0.8f : 0.2f);
            std::memcpy(s.l, s.inL, sizeof s.l);
            std::memcpy(s.r, s.inR, sizeof s.r);
            e.process(s.l, s.r, ctx);
            bench::doNotOptimize(s.l[3]);
        });
}

void benchSixteen(bench::State& state, bool silent)
{
    Stimulus s;
    ProcessContext ctx{44100.0, 120.0, true, 0u, {}, 0};
    ctx.triggerOffset.fill(-1);
    std::unique_ptr<Reverb> e[16];
    for (auto& x : e)
    {
        x = std::make_unique<Reverb>();
        setUp(*x, silent ? 0.3f : 0.6f);
    }
    if (silent) // a burst, then 15 s of silence: every tail is asleep (−120 dBFS ≈ 7 s at T60 3.55 s)
        for (int b = 0; b < 10400; ++b)
            for (auto& x : e)
            {
                std::memcpy(s.l, s.inL, sizeof s.l);
                std::memcpy(s.r, s.inR, sizeof s.r);
                if (b >= 10)
                {
                    std::memset(s.l, 0, sizeof s.l);
                    std::memset(s.r, 0, sizeof s.r);
                }
                x->process(s.l, s.r, ctx);
            }
    state.run(
        [&]
        {
            for (auto& x : e)
            {
                if (silent)
                {
                    std::memset(s.l, 0, sizeof s.l);
                    std::memset(s.r, 0, sizeof s.r);
                }
                else
                {
                    std::memcpy(s.l, s.inL, sizeof s.l);
                    std::memcpy(s.r, s.inR, sizeof s.r);
                }
                x->process(s.l, s.r, ctx);
                bench::doNotOptimize(s.l[3]);
            }
        });
}
} // namespace

BENCH(fx_reverb, 14000)
{
    benchReverb(state, true);
}

BENCH(fx_reverb_steady, 14000)
{
    benchReverb(state, false);
}

BENCH(fx_reverb_16, 224000)
{
    benchSixteen(state, false);
}

BENCH(fx_reverb_16_idle, 8000)
{
    benchSixteen(state, true);
}
