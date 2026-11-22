// Host side of the bench harness: each BENCH is a Catch2 test case (label `bench`) that runs the bench, prints its
// budget-vs-measured line, writes <artifacts>/bench/<name>.json for tests/bench/harness/check_regression.py and fails
// when the median exceeds the budget. Not compiled into the device bench (BENCH_STANDALONE).
#if ! defined(BENCH_STANDALONE)
#include "tests/bench/harness/Bench.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace bench::detail
{
void writeJson(const Result& r)
{
#if defined(APP_ARTIFACT_DIR)
    const std::filesystem::path dir = std::filesystem::path(APP_ARTIFACT_DIR) / "bench";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string path = (dir / (std::string(r.name) + ".json")).string();
    // Repeated runs into the same folder (ci/run-local.sh bench runs the suite 3 times) keep the fastest: the least
    // noisy figure on a busy or shared host.
    if (std::FILE* old = std::fopen(path.c_str(), "r"))
    {
        char text[512] = {};
        const std::size_t n = std::fread(text, 1, sizeof text - 1, old);
        std::fclose(old);
        text[n] = 0;
        double previous = 0;
        if (const char* at = std::strstr(text, "\"min_cycles\": "))
            if (std::sscanf(at + 14, "%lf", &previous) == 1 && previous > 0 && previous <= r.minCycles)
                return;
    }
    if (std::FILE* f = std::fopen(path.c_str(), "w"))
    {
        std::fprintf(f,
                     "{\"name\": \"%s\", \"budget_cycles\": %.1f, \"median_cycles\": %.3f, \"p95_cycles\": %.3f, "
                     "\"min_cycles\": %.3f, \"batch\": %llu, \"samples\": %d}\n",
                     r.name, r.budgetCycles, r.medianCycles, r.p95Cycles, r.minCycles,
                     static_cast<unsigned long long>(r.batch), r.samples);
        std::fclose(f);
    }
#else
    (void)r;
#endif
}

void catchRun(const char* name)
{
    const Entry* e = find(name);
    REQUIRE(e != nullptr);
    const Result r = run(*e);
    char line[512];
    format(r, line, sizeof line);
    std::printf("%s\n", line);
    std::fflush(stdout);
    REQUIRE(r.ran);
    writeJson(r);
    INFO(line);
    CHECK(r.medianCycles <= r.budgetCycles);
}
} // namespace bench::detail

namespace bench
{
void record(const char* name, double budgetCycles, double measuredCycles, double p95Cycles)
{
    Result r;
    r.name = name;
    r.budgetCycles = budgetCycles;
    r.medianCycles = r.minCycles = measuredCycles;
    r.p95Cycles = p95Cycles < 0 ? measuredCycles : p95Cycles;
    r.batch = 1;
    r.samples = 1;
    r.ran = true;
    char line[512];
    format(r, line, sizeof line);
    std::printf("%s\n", line);
    std::fflush(stdout);
    detail::writeJson(r);
}
} // namespace bench
#endif
