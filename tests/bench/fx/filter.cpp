// Multi-mode filter bench (budget: filter ≤ 9k A9 cycles per 64-frame stereo block,
// worst case 24 dB/oct = two cascaded biquads with the cutoff and Q gliding). One iteration = one block through
// MultiModeFilter::process (the NEON path on the device), the cutoff and Q re-targeted every block so a glide step (and
// its coefficient recomputation) lands in every block, and the output gain ramping half of the time.
//   fx_filter          24 dB, Glide 1, cutoff/Q/gain changes every block      ≤ 9000
//   fx_filter_steady   12 dB, settled (no glide, no ramp)                     ≤ 4500
// Desktop timings are relative only; the device numbers come from scripts/emulator-bench.sh fx_filter.
#include "tests/bench/harness/Bench.h"

#include "engine/fx/effects/MultiModeFilter.h"

#include <cmath>
#include <cstring>

namespace
{
using engine::fx::effects::MultiModeFilter;
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

void setUp(MultiModeFilter& f, int slope, float glide)
{
    f.prepare(44100.0, N);
    for (int k = 0; k < 6; ++k)
        f.setParam(k, MultiModeFilter::kDefaults[k]);
    f.setParam(3, glide);
    f.setOption(MultiModeFilter::kOptType, MultiModeFilter::kLowPass);
    f.setOption(MultiModeFilter::kOptSlope, slope);
    f.reset();
}
} // namespace

BENCH(fx_filter, 9000)
{
    MultiModeFilter f;
    setUp(f, MultiModeFilter::k24dB, 1.0f);
    const auto ctx = context();
    Io io;
    int n = 0;
    state.run(
        [&]
        {
            io.fill(n & 7);
            const bool odd = (n & 1) != 0;
            f.setParam(0, odd ? 0.25f : 0.85f);
            f.setParam(2, odd ? 0.2f : 0.6f);
            f.setParam(1, (n & 2) != 0 ? 0.6f : 0.75f);
            ++n;
            f.process(io.l, io.r, ctx);
            bench::doNotOptimize(io.l[5]);
        });
}

BENCH(fx_filter_steady, 4500)
{
    MultiModeFilter f;
    setUp(f, MultiModeFilter::k12dB, 0.0f);
    const auto ctx = context();
    Io io;
    for (int b = 0; b < 16; ++b) // settle the start-up and the first glide
    {
        io.fill(b);
        f.process(io.l, io.r, ctx);
    }
    int n = 0;
    state.run(
        [&]
        {
            io.fill(n++ & 7);
            f.process(io.l, io.r, ctx);
            bench::doNotOptimize(io.l[5]);
        });
}
