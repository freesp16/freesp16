// Benches of the bench harness itself and of the engine LoadMeter.
//   bench_harness_selftest  a fixed workload that proves the harness end to end on the host and in the emulator: a
//                           64-frame stereo gain ramp (the shape of a track strip's fader; guide ≤ 1.5 k cycles/block)
//   load_meter_begin_end    the LoadMeter's own cost per block: begin() + end() (two clock reads + relaxed stores).
//                           Budget 2 k cycles: on the device's kernel clock_gettime is a syscall (no ARM vDSO before
//                           Linux 4.1), about 0.3–1 µs each on a Cortex-A9 (hardware number open)
//   load_meter_snapshot     the message-thread snapshot (p95 over the fixed histogram; once per LOAD line)
//   bench_harness_engine_block  one 64-frame block of the empty engine (engine::Engine, test tone off) through
//                           process(), the LoadMeter included — the BENCH() twin of the Catch2 case engine_empty
//                           (budget ≤ 15 k), so the engine frame also runs in the emulator (scripts/emulator-bench.sh)
#include "tests/bench/harness/Bench.h"

#include "engine/Engine.h"
#include "engine/LoadMeter.h"

#include <array>
#include <memory>

namespace
{
constexpr int kFrames = 64;

struct Strip
{
    alignas(16) std::array<float, kFrames> inL{}, inR{}, outL{}, outR{};
    float gain = 0.5f, target = 0.8f;

    void fill()
    {
        for (int i = 0; i < kFrames; ++i)
        {
            inL[static_cast<std::size_t>(i)] = static_cast<float>(i % 17) * 0.01f - 0.08f;
            inR[static_cast<std::size_t>(i)] = static_cast<float>(i % 13) * -0.01f + 0.06f;
        }
    }
    void process() noexcept
    {
        const float step = (target - gain) * (1.0f / kFrames);
        float g = gain;
        for (int i = 0; i < kFrames; ++i)
        {
            g += step;
            outL[static_cast<std::size_t>(i)] = inL[static_cast<std::size_t>(i)] * g;
            outR[static_cast<std::size_t>(i)] = inR[static_cast<std::size_t>(i)] * g;
        }
        gain = g;
        target = target > 0.7f ? 0.3f : 0.9f; // keep ramping, never converge
    }
};
} // namespace

BENCH(bench_harness_selftest, 1500)
{
    Strip strip;
    strip.fill();
    state.run(
        [&]
        {
            strip.process();
            bench::doNotOptimize(strip.outL[7]);
            bench::doNotOptimize(strip.outR[63]);
        });
}

BENCH(load_meter_begin_end, 2000)
{
    static engine::LoadMeter meter; // ~6 KB of histogram: static, like the engine member it is
    meter.prepare(44100.0, kFrames);
    state.run(
        [&]
        {
            meter.begin();
            bench::clobberMemory();
            meter.end(kFrames);
        });
}

BENCH(load_meter_snapshot, 20000)
{
    static engine::LoadMeter meter;
    meter.prepare(44100.0, kFrames);
    for (int i = 0; i < 2000; ++i)
    {
        meter.begin();
        meter.end(kFrames);
    }
    state.run(
        [&]
        {
            const auto s = meter.snapshot();
            bench::doNotOptimize(s.p95Cycles);
        });
}

BENCH(bench_harness_engine_block, 15000)
{
    engine::EngineOptions options;
    options.testTone = false;
    auto e = std::make_unique<engine::Engine>(engine::EngineSetup{}, options);
    e->prepare(44100.0, kFrames);
    alignas(16) std::array<float, kFrames> l{}, r{};
    float* out[2] = {l.data(), r.data()};
    state.run(
        [&]
        {
            e->process(out, 2, kFrames);
            bench::doNotOptimize(l[0]);
        });
}
