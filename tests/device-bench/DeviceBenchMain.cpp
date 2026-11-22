// The device bench's main (tests/device-bench/CMakeLists.txt): the host bench suite under Catch2 on the
// reference hardware. Output, one line each, for tools/bench-report.py:
//   BENCH DEVICE START <name> <version> args=<…>                 then `BENCH DIAG …` (kernel, cpu + governor, libc,
//                                                                memory: the conditions the numbers were taken under)
//   BENCHCASE START <test case>                                  before each Catch2 test case (a hang or crash is
//                                                                attributed to the case after the last START)
//   BENCH <name> budget=<cycles> measured=<cycles> (…) … OK|OVER  every BENCH() and bench::record (Bench.h)
//   BENCHCASE <PASS|FAIL> <test case> assertions=<n> failed=<n> ms=<ms>
//   BENCH DONE rc=<Catch2's exit code> cases=<n> failed=<n> seconds=<s>
// Arguments are Catch2's (a test name, tags such as "[mixer]", wildcards such as "bench_mixer*", --list-tests …); no
// arguments = every non-hidden test case. One cycle = one nanosecond of CLOCK_MONOTONIC_RAW = one A9 cycle at 1 GHz:
// check `BENCH DIAG cpu … freq_khz=… governor=…` (a lower clock or the ondemand governor makes the numbers larger).
#include "app/Diagnostics.h"

#include "ProjectIdentity.h"

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_case_info.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

const char* deviceBenchScratchDir() noexcept
{
    static const std::string dir = []
    {
        const char* v = std::getenv("BENCH_TMPDIR");
        return v != nullptr && *v != 0 ? std::string(v) : std::string("/tmp/") + identity::name + "-bench";
    }();
    return dir.c_str();
}

namespace
{
double nowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

int gCases = 0, gFailed = 0;

class BenchCaseListener final : public Catch::EventListenerBase
{
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testCaseStarting(const Catch::TestCaseInfo& info) override
    {
        start_ = nowMs();
        std::printf("BENCHCASE START %s\n", info.name.c_str());
        std::fflush(stdout);
    }
    void testCaseEnded(const Catch::TestCaseStats& stats) override
    {
        const bool ok = stats.totals.assertions.failed == 0 && stats.totals.testCases.failed == 0;
        ++gCases;
        gFailed += ok ? 0 : 1;
        std::printf("BENCHCASE %s %s assertions=%llu failed=%llu ms=%.0f\n", ok ? "PASS" : "FAIL",
                    stats.testInfo->name.c_str(),
                    static_cast<unsigned long long>(stats.totals.assertions.total()),
                    static_cast<unsigned long long>(stats.totals.assertions.failed), nowMs() - start_);
        std::fflush(stdout);
    }

private:
    double start_ = 0;
};
} // namespace

CATCH_REGISTER_LISTENER(BenchCaseListener)

int main(int argc, char** argv)
{
    app::diag::lineBufferStdout();
    app::diag::installCrashHandler("BENCH");
    std::string args;
    for (int i = 1; i < argc; ++i)
        args += std::string(i > 1 ? " " : "") + argv[i];
    std::printf("BENCH DEVICE START %s %s args=%s scratch=%s\n", identity::name, identity::version,
                args.empty() ? "(all)" : args.c_str(), deviceBenchScratchDir());
    platform::Paths paths;
    paths.dataDir = "/tmp";
    paths.cacheDir = "/tmp";
    app::diag::systemLines(paths, [](const std::string& line) { std::printf("BENCH DIAG %s\n", line.c_str()); });
    std::fflush(stdout);
    const double t0 = nowMs();
    Catch::Session session;
    const int rc = session.run(argc, argv);
    std::printf("BENCH DONE rc=%d cases=%d failed=%d seconds=%.0f\n", rc, gCases, gFailed, (nowMs() - t0) / 1000.0);
    std::fflush(stdout);
    return rc;
}
