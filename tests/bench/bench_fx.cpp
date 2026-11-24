// Effects DSP kit kernels, one iteration = one 64-frame stereo block:
//   bench_fx_kernel_mix        the non-fused dry/wet mix, both channels            ≤ 200
//   bench_fx_kernel_hpf        the shell's 10 Hz high-pass, L/R in two lanes      ≤ 700
//   bench_fx_kernel_biquad     Biquad2 DF1, L/R in two lanes                       ≤ 900
//   bench_fx_kernel_svf        Svf (TPT), stereo, scalar                           ≤ 1200
//   bench_fx_kernel_delay      DelayLine block read + write (integer delay)        ≤ 300
//   bench_fx_kernel_delay_frac DelayLine 64 linear-interpolated stereo reads       ≤ 1200
//   bench_fx_kernel_osc        Oscillator: 64 sine samples (table + linear)       ≤ 600
// Host only (Catch2): bench_fx_effects times every registered effect in every slot kind it allows through a running
// FxProcessor (shell included), against the 9x envelope (insert ≤ 8 k, Send Delay ≤ 12 k, Reverb ≤ 40 k, others
// ≤ 8 k), one line per effect: a new effect is benchmarked by registering it, no edit here.
#include "tests/bench/harness/Bench.h"

#include "engine/fx/EffectRegistryOrder.h"
#include "engine/fx/FxProcessor.h"
#include "engine/fx/dsp/DelayLine.h"
#include "engine/fx/dsp/Filters.h"
#include "engine/fx/dsp/Oscillator.h"
#include "engine/fx/dsp/Vec.h"

#include <cmath>
#include <memory>
#include <string>

namespace
{
using namespace engine::fx;
constexpr int N = engine::kBlockFrames;

struct Buffers
{
    alignas(16) float l[N];
    alignas(16) float r[N];
    alignas(16) float cl[N];
    alignas(16) float cr[N];
    Buffers()
    {
        for (int i = 0; i < N; ++i)
        {
            l[i] = 0.5f * std::sin(0.1f * static_cast<float>(i));
            r[i] = 0.4f * std::cos(0.07f * static_cast<float>(i));
            cl[i] = 0.3f * std::sin(0.2f * static_cast<float>(i));
            cr[i] = 0.2f * std::cos(0.3f * static_cast<float>(i));
        }
    }
};
} // namespace

BENCH(bench_fx_kernel_mix, 200)
{
    Buffers b;
    state.run(
        [&]
        {
            dsp::vec::mix(b.l, b.cl, 0.5f, 0.5f, N);
            dsp::vec::mix(b.r, b.cr, 0.5f, 0.5f, N);
            bench::doNotOptimize(b.l[3]);
        });
}

BENCH(bench_fx_kernel_hpf, 700)
{
    Buffers b;
    dsp::Hpf1 h;
    h.prepare(44100.0);
    h.setCutoff(10.0f);
    state.run(
        [&]
        {
            h.process(b.l, b.r, N);
            bench::doNotOptimize(b.l[3]);
        });
}

BENCH(bench_fx_kernel_biquad, 900)
{
    Buffers b;
    dsp::Biquad2 q;
    q.setCoefs(dsp::BiquadCoefs::design(dsp::BiquadCoefs::Type::Peaking, 44100.0, 1000.0, 0.7, 3.0));
    state.run(
        [&]
        {
            q.process(b.l, b.r, N);
            bench::doNotOptimize(b.l[3]);
        });
}

BENCH(bench_fx_kernel_svf, 1200)
{
    Buffers b;
    dsp::Svf s;
    s.setParams(44100.0, 800.0, 0.9);
    state.run(
        [&]
        {
            s.process(dsp::Svf::Mode::LowPass, b.l, b.r, N);
            bench::doNotOptimize(b.l[3]);
        });
}

BENCH(bench_fx_kernel_delay, 300)
{
    Buffers b;
    auto d = std::make_unique<dsp::DelayLine>();
    d->prepare(48000 + 2500);
    for (int k = 0; k < 800; ++k)
        d->writeBlock(b.l, b.r, N);
    state.run(
        [&]
        {
            d->readBlock(b.cl, b.cr, 22050, N);
            d->writeBlock(b.l, b.r, N);
            bench::doNotOptimize(b.cl[3]);
        });
}

BENCH(bench_fx_kernel_delay_frac, 1200)
{
    Buffers b;
    auto d = std::make_unique<dsp::DelayLine>();
    d->prepare(5000);
    for (int k = 0; k < 100; ++k)
        d->writeBlock(b.l, b.r, N);
    float age = 300.25f;
    state.run(
        [&]
        {
            for (int i = 0; i < N; ++i)
                d->readLinear(age + 0.01f * static_cast<float>(i), b.cl[i], b.cr[i]);
            d->writeBlock(b.l, b.r, N);
            bench::doNotOptimize(b.cl[3]);
        });
}

BENCH(bench_fx_kernel_osc, 600)
{
    Buffers b;
    dsp::Oscillator o;
    o.setFrequency(44100.0, 3.7);
    state.run(
        [&]
        {
            o.render(b.cl, N);
            bench::doNotOptimize(b.cl[3]);
        });
}

#if ! defined(BENCH_STANDALONE)
TEST_CASE("bench_fx_effects", "[bench]")
{
    const auto& reg = EffectRegistry::builtin();
    int timed = 0;
    for (const auto& d : reg.all())
        for (auto kind : {SlotKind::Insert, SlotKind::Send, SlotKind::Master})
        {
            if (! allows(d.slots, kind))
                continue;
            const EffectDescriptor* types[] = {&d};
            auto p = std::make_unique<FxProcessor>();
            p->build(kind, types);
            REQUIRE(p->prepare(44100.0, N));
            for (int k = 0; k < 6; ++k)
                p->setParam(0, k, 0.5f);
            p->requestType(0);
            Buffers b;
            ProcessContext ctx{44100.0, 120.0, true, 0u, {}, 0};
            ctx.triggerOffset.fill(-1);
            for (int k = 0; k < 8; ++k)
                p->process(b.l, b.r, ctx);
            bench::State st;
            st.run(
                [&]
                {
                    p->process(b.l, b.r, ctx);
                    bench::doNotOptimize(b.l[3]);
                });
            const double budget = d.id == "delayBus" ? 12000.0 : 8000.0;
            const std::string name =
                "bench_fx_effect_" + std::string(d.id) + "_" +
                (kind == SlotKind::Insert ? "insert" : (kind == SlotKind::Send ? "send" : "master"));
            const auto r = st.result(name.c_str(), budget);
            bench::record(name.c_str(), budget, r.medianCycles, r.p95Cycles);
            CHECK(r.medianCycles <= budget);
            ++timed;
        }
    CHECK(timed >= 2);
}
#endif
