// engine::LoadMeter: exact statistics from injected durations, window rotation, overruns,
// frame scaling, and the real-time contract — begin()/end() on a thread marked real-time under the allocation hook
// (host preset: APP_RT_CHECKS aborts on any allocation) while another thread takes snapshots.
#include "core/util/Realtime.h"
#include "engine/LoadMeter.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace engine
{
struct LoadMeterTestAccess
{
    static void record(LoadMeter& m, std::uint64_t ns, int frames) { m.record(ns, frames); }
    static int binOf(std::uint32_t c) { return LoadMeter::binOf(c); }
    static double binValue(int b) { return LoadMeter::binValue(b); }
    static constexpr int bins = LoadMeter::kBins;
    static constexpr std::uint32_t maxValue = LoadMeter::kMaxValue;
};
} // namespace engine

using engine::LoadMeter;
using Access = engine::LoadMeterTestAccess;
using Catch::Approx;

namespace
{
constexpr double kRate = 44100.0;
constexpr int kBlock = 64;
constexpr double kPeriod = 1.0e9 * kBlock / kRate; // 1451247 ns = cycles at 1 GHz

std::unique_ptr<LoadMeter> prepared()
{
    auto m = std::make_unique<LoadMeter>();
    m->prepare(kRate, kBlock);
    return m;
}
} // namespace

TEST_CASE("load meter: histogram bins are monotonic, contiguous and within 3.2 %", "[perf]")
{
    int previous = -1;
    for (std::uint32_t c = 0; c < 200000; c += (c < 4096 ? 1 : 97))
    {
        const int b = Access::binOf(c);
        REQUIRE(b >= previous);
        REQUIRE(b <= previous + 1);
        if (c > 0)
            REQUIRE(Access::binValue(b) == Approx(c).epsilon(0.032));
        previous = b;
    }
    CHECK(Access::binOf(Access::maxValue) == Access::bins - 1);
    for (std::uint32_t c = 0; c < 64; ++c)
        CHECK(Access::binValue(Access::binOf(c)) == c); // exact below 64 cycles
}

TEST_CASE("load meter: avg, p95, max, load % from known durations", "[perf]")
{
    auto m = prepared();
    CHECK(m->snapshot().blocks == 0);
    CHECK(m->snapshot().avgCycles == 0);
    for (int i = 0; i < 95; ++i)
        Access::record(*m, 10000, kBlock);
    for (int i = 0; i < 5; ++i)
        Access::record(*m, 100000, kBlock);
    const auto s = m->snapshot();
    CHECK(s.blocks == 100);
    CHECK(s.overruns == 0);
    CHECK(s.avgCycles == Approx(14500.0));
    CHECK(s.p95Cycles == Approx(10000.0).epsilon(0.032));
    CHECK(s.maxCycles == Approx(100000.0));
    CHECK(s.loadPercent == Approx(100.0 * 14500.0 / kPeriod));

    Access::record(*m, 100000, kBlock); // 6 % slow now: p95 moves to the slow cluster
    CHECK(m->snapshot().p95Cycles == Approx(100000.0).epsilon(0.032));
}

TEST_CASE("load meter: overruns, scaling to the engine block, prepare resets", "[perf]")
{
    auto m = prepared();
    Access::record(*m, static_cast<std::uint64_t>(kPeriod * 1.5), kBlock); // 150 % of a 64-frame block
    Access::record(*m, static_cast<std::uint64_t>(kPeriod * 0.9), kBlock);
    Access::record(*m, 2000, 4 * kBlock); // one 256-frame device period = 500 cycles per 64-frame block
    auto s = m->snapshot();
    CHECK(s.blocks == 3);
    CHECK(s.overruns == 1);
    CHECK(s.maxCycles == Approx(kPeriod * 1.5).epsilon(1e-6));
    CHECK(s.avgCycles == Approx((kPeriod * 1.5 + kPeriod * 0.9 + 500.0) / 3).epsilon(1e-6));
    Access::record(*m, 4000, 0); // no frames: ignored
    CHECK(m->snapshot().blocks == 3);

    m->prepare(48000.0, 128);
    s = m->snapshot();
    CHECK(s.blocks == 0);
    CHECK(s.overruns == 0);
    CHECK(s.maxCycles == 0);
}

TEST_CASE("load meter: statistics cover the current and the previous 10 s window", "[perf]")
{
    auto m = prepared();
    const int blocksPerWindow = static_cast<int>(10.0 * kRate / kBlock) + 1; // 6891
    for (int i = 0; i < blocksPerWindow; ++i)
        Access::record(*m, 500000, kBlock); // window 1: slow
    for (int i = 0; i < blocksPerWindow - 1; ++i)
        Access::record(*m, 1000, kBlock); // window 2: fast, one block short of full — window 1 still counts
    CHECK(m->snapshot().maxCycles == Approx(500000.0));
    for (int i = 0; i < 10; ++i)
        Access::record(*m, 2000, kBlock); // the first completes window 2; window 3 starts and window 1 is dropped
    const auto s = m->snapshot();
    CHECK(s.maxCycles == Approx(2000.0));
    CHECK(s.p95Cycles == Approx(1000.0).epsilon(0.032));
    CHECK(s.blocks == static_cast<std::uint64_t>(2 * blocksPerWindow - 1 + 10)); // cumulative since prepare
}

TEST_CASE("load meter: real clock, real-time thread, no allocation, concurrent snapshots", "[perf][realtime]")
{
    auto m = prepared();
    std::atomic<bool> done{false};
    std::uint64_t lastBlocks = 0;
    bool monotonic = true;
    std::thread reader(
        [&]
        {
            while (! done.load())
            {
                const auto s = m->snapshot();
                monotonic = monotonic && s.blocks >= lastBlocks && s.p95Cycles <= s.maxCycles + 1e-9;
                lastBlocks = s.blocks;
            }
        });
    {
        core::rt::ScopedRealtimeThread realtime; // any allocation in begin()/end() aborts (APP_RT_CHECKS)
        for (int i = 0; i < 20000; ++i)
        {
            m->begin();
            if (i % 1000 == 0) // a few slow blocks: ~200 µs busy wait
            {
                const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(200);
                while (std::chrono::steady_clock::now() < until)
                {
                }
            }
            m->end(kBlock);
        }
    }
    done = true;
    reader.join();
    const auto s = m->snapshot();
    CHECK(monotonic);
    CHECK(s.blocks == 20000);
    CHECK(s.maxCycles >= 200000.0);
    CHECK(s.avgCycles > 0);
    CHECK(s.p95Cycles < 200000.0); // 20 of 20000 blocks are slow: not the 95th percentile
    CHECK(s.overruns <= 200);      // 200 µs < 1.45 ms; ≤ 1 % margin for preemption on a loaded host (no RT kernel)
#if APP_RT_CHECKS
    CHECK(core::rt::violationCount() == 0);
#endif
}
