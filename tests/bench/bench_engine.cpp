// Engine frame benchmarks (performance budget: "empty engine (16 silent tracks) ≤ 15 k cycles per
// block"). Release only (host-release preset): `ctest --preset host-release -L bench -R engine_empty`.
//
// Cycles are host nanoseconds normalised to the A9's 1 GHz (1 ns = 1 cycle; the harness convention). The host is far
// faster than a Cortex-A9, so these numbers rank and guard against regressions; the device numbers come from the
// LoadMeter / scripts/emulator-bench.sh on hardware (open until measured there). These cases may move to the BENCH()
// harness (harness/Bench.h); the budgets stay.
#include "engine/Engine.h"
#include "engine/dsp/simd/Kernels.h"
#include "tests/bench/harness/Bench.h"

#include "core/util/Realtime.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

namespace
{
struct Stats
{
    double avg, p50, p95, max;
};

Stats stats(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v)
        sum += x;
    const auto at = [&](double q) { return v[static_cast<std::size_t>(q * static_cast<double>(v.size() - 1))]; };
    return {sum / static_cast<double>(v.size()), at(0.5), at(0.95), v.back()};
}

// Times `blocks` engine blocks, one 64-frame device period each (the adapter's cheapest path; a 256-frame period
// costs the same per block plus the copies).
Stats timeBlocks(engine::Engine& e, int blocks, int period)
{
    using clock = std::chrono::steady_clock;
    std::vector<float> l(static_cast<std::size_t>(period)), r(static_cast<std::size_t>(period));
    float* out[2] = {l.data(), r.data()};
    std::vector<double> ns;
    ns.reserve(static_cast<std::size_t>(blocks));
    for (int i = 0; i < 200; ++i) // warm caches and branch predictors
        e.process(out, 2, period);
    for (int i = 0; i < blocks; ++i)
    {
        const auto t0 = clock::now();
        e.process(out, 2, period);
        const auto t1 = clock::now();
        ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() * 64.0 / period);
    }
    return stats(std::move(ns));
}

void report(const char* name, double budget, const Stats& s)
{
    std::printf("BENCH %s budget=%.0f cycles/block measured avg=%.0f p50=%.0f p95=%.0f max=%.0f (host ns @ 1 GHz)\n", name,
                budget, s.avg, s.p50, s.p95, s.max);
    bench::record(name, budget, s.p50, s.p95); // standard line + regression baseline
}

engine::EngineSetup noParams() { return {}; }
} // namespace

TEST_CASE("engine_empty: 16 silent tracks, 2 idle inserts each, per 64-frame block", "[bench][engine]")
{
    engine::EngineOptions o;
    o.testTone = false;
    engine::Engine e(noParams(), o);
    e.prepare(44100.0, 256);
    const auto s = timeBlocks(e, 20000, 64);
    report("engine_empty", 15000, s);
    CHECK(s.avg <= 15000.0);
    CHECK(s.p50 <= 15000.0);
    const auto s256 = timeBlocks(e, 5000, 256);
    report("engine_empty_period256", 15000, s256);
    CHECK(s256.avg <= 15000.0);
}

TEST_CASE("engine_tone: the temporary test tone on track 1", "[bench][engine]")
{
    engine::Engine e;
    e.prepare(44100.0, 256);
    const auto s = timeBlocks(e, 20000, 64);
    report("engine_tone", 15000 + 6000, s); // empty engine + one voice (budget: voice ≤ 6 k)
    CHECK(s.avg <= 21000.0);
}

TEST_CASE("engine_meters: NEON peak + sum of squares vs scalar, 2 × 64 frames", "[bench][engine]")
{
    alignas(16) float block[2][64];
    for (int i = 0; i < 64; ++i)
        block[0][i] = block[1][i] = static_cast<float>(i % 7) * 0.1f - 0.3f;
    using clock = std::chrono::steady_clock;
    volatile float sink = 0;
    constexpr int reps = 200000;
    auto t0 = clock::now();
    for (int i = 0; i < reps; ++i)
        sink = sink + engine::simd::peakSum(block[0], 64).peak + engine::simd::peakSum(block[1], 64).sumSquares;
    auto t1 = clock::now();
    for (int i = 0; i < reps; ++i)
        sink = sink + engine::simd::scalar::peakSum(block[0], 64).peak + engine::simd::scalar::peakSum(block[1], 64).sumSquares;
    auto t2 = clock::now();
    const double active = std::chrono::duration<double, std::nano>(t1 - t0).count() / reps;
    const double scalar = std::chrono::duration<double, std::nano>(t2 - t1).count() / reps;
    std::printf("BENCH engine_meters budget=500 cycles/block measured %s=%.1f scalar=%.1f (host ns @ 1 GHz)\n",
                engine::simd::hasNeon() ? "neon" : "scalar", active, scalar);
    bench::record("engine_meters", 500, active);
    bench::record("engine_meters_scalar", 500, scalar);
    CHECK(active <= 500.0);
}
