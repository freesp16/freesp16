// 2-band EQ bench (budget: eq2 ≤ 9k A9 cycles per 64-frame stereo block, worst
// case two biquad bands with a glide every block). One iteration = one block through ParametricEq::process (the NEON path
// on the device); both bands are peaks whose frequency, Q and gain are re-targeted every block (inside the glide limits,
// so they glide instead of jumping): a glide step with its double-precision coefficient set-up and a gain ramp in both
// bands in every block.
//   fx_eq2          two peak bands, every parameter gliding                  ≤ 9000
//   fx_eq2_steady   low shelf + high shelf, settled                          ≤ 4500
// Desktop timings are relative only; the device numbers come from scripts/emulator-bench.sh fx_eq2.
#include "tests/bench/harness/Bench.h"

#include "engine/fx/effects/ParametricEq.h"

#include <cmath>
#include <cstring>

namespace
{
using engine::fx::effects::ParametricEq;
constexpr int N = engine::kBlockFrames;

engine::fx::ProcessContext context()
{
    engine::fx::ProcessContext c{44100.0, 120.0, true, 0u, {}, 0};
    c.triggerOffset.fill(-1);
    return c;
}

struct Io // 8 blocks of test signal computed once; fill() copies one (≈ 30 ns, not the sin/cos)
{
    alignas(16) float l[N];
    alignas(16) float r[N];
    alignas(16) float srcL[8][N];
    alignas(16) float srcR[8][N];
    Io()
    {
        for (int n = 0; n < 8; ++n)
            for (int i = 0; i < N; ++i)
            {
                srcL[n][i] = 0.5f * std::sin(0.031f * static_cast<float>(i + n * N));
                srcR[n][i] = 0.4f * std::cos(0.017f * static_cast<float>(i + n * N));
            }
    }
    void fill(int n)
    {
        std::memcpy(l, srcL[n & 7], sizeof l);
        std::memcpy(r, srcR[n & 7], sizeof r);
    }
};

void setUp(ParametricEq& e, int t1, int t2)
{
    e.prepare(44100.0, N);
    for (int k = 0; k < 6; ++k)
        e.setParam(k, ParametricEq::kDefaults[k]);
    e.setOption(ParametricEq::kOptType1, t1);
    e.setOption(ParametricEq::kOptType2, t2);
    e.reset();
}
} // namespace

BENCH(fx_eq2, 9000)
{
    ParametricEq e;
    setUp(e, ParametricEq::kAppPeak, ParametricEq::kAppPeak);
    const auto ctx = context();
    Io io;
    int n = 0;
    state.run(
        [&]
        {
            io.fill(n & 7);
            const bool odd = (n & 1) != 0;
            for (int b = 0; b < 2; ++b)
            {
                e.setParam(3 * b + 0, odd ? 0.50f : 0.55f + 0.1f * static_cast<float>(b)); // ≤ 38 frequency steps
                e.setParam(3 * b + 1, odd ? 0.60f : 0.70f);                                  // 4.5 dB: 113 gain steps
                e.setParam(3 * b + 2, odd ? 0.10f : 0.20f);                                  // ≤ 70 Q steps
            }
            ++n;
            e.process(io.l, io.r, ctx);
            bench::doNotOptimize(io.l[5]);
        });
}

BENCH(fx_eq2_steady, 4500)
{
    ParametricEq e;
    setUp(e, ParametricEq::kAppLoShelf, ParametricEq::kAppHiShelf);
    e.setParam(1, 0.8f);
    e.setParam(4, 0.5f);
    const auto ctx = context();
    Io io;
    for (int b = 0; b < 400; ++b) // settle both glides (gain −7.5 dB: 188 steps)
    {
        io.fill(b & 7);
        e.process(io.l, io.r, ctx);
    }
    int n = 0;
    state.run(
        [&]
        {
            io.fill(n++ & 7);
            e.process(io.l, io.r, ctx);
            bench::doNotOptimize(io.l[5]);
        });
}
