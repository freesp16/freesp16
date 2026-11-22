// core::CpuUsage: the /proc/stat parser, the busy share between two samples (clamps, no elapsed tick), and a live
// sample on a machine that has the counters.
#include "core/util/CpuUsage.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>

TEST_CASE("cpu usage: parses the aggregate cpu line of /proc/stat", "[perf][cpu]")
{
    std::uint64_t busy = 0, total = 0;
    // user nice system idle iowait irq softirq steal guest guest_nice (guest is part of user: not added twice)
    REQUIRE(core::CpuUsage::parseProcStat("cpu  100 20 30 800 40 5 3 2 50 7\ncpu0 1 2 3 4\n", busy, total));
    CHECK(total == 1000);
    CHECK(busy == 160);
    // An old kernel's four fields.
    REQUIRE(core::CpuUsage::parseProcStat("cpu 10 0 10 80\n", busy, total));
    CHECK(total == 100);
    CHECK(busy == 20);
    CHECK_FALSE(core::CpuUsage::parseProcStat("cpu0 1 2 3 4\n", busy, total));
    CHECK_FALSE(core::CpuUsage::parseProcStat("intr 1 2 3\n", busy, total));
    CHECK_FALSE(core::CpuUsage::parseProcStat("cpu  1 2\n", busy, total));
}

TEST_CASE("cpu usage: busy share between two samples", "[perf][cpu]")
{
    CHECK(core::CpuUsage::share(100, 1000, 350, 2000) == Catch::Approx(25.0));
    CHECK(core::CpuUsage::share(100, 1000, 1100, 2000) == Catch::Approx(100.0));
    CHECK(core::CpuUsage::share(100, 1000, 100, 2000) == Catch::Approx(0.0));
    CHECK(core::CpuUsage::share(100, 1000, 90, 2000) == Catch::Approx(0.0));   // busy stepped back: 0
    CHECK(core::CpuUsage::share(100, 1000, 5000, 2000) == Catch::Approx(100.0)); // more busy than elapsed: 100
    CHECK(core::CpuUsage::share(100, 1000, 200, 1000) < 0);                      // no tick elapsed
}

TEST_CASE("cpu usage: a live sample is a share once a tick has elapsed", "[perf][cpu]")
{
    core::CpuUsage cpu;
    CHECK(cpu.percent() < 0);
    if (! cpu.sample())
        SKIP("no CPU counters on this system");
    CHECK(cpu.percent() < 0); // the baseline only
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    volatile unsigned spin = 0;
    while (std::chrono::steady_clock::now() < until) // busy for 100 ms: at least one tick on this core
        spin = spin + 1;
    REQUIRE(cpu.sample());
    CHECK(cpu.percent() > 0);
    CHECK(cpu.percent() <= 100);
    cpu.reset();
    CHECK(cpu.percent() < 0);
}
