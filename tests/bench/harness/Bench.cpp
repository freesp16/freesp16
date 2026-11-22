// Bench harness: clock, registry, run, report line (host and device; see Bench.h).
#include "tests/bench/harness/Bench.h"

#include "core/util/Realtime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#else
#include <time.h>
#endif

namespace bench
{
namespace
{
constexpr int kMaxEntries = 1024;
std::array<Entry, kMaxEntries>& table() noexcept
{
    static std::array<Entry, kMaxEntries> entries{};
    return entries;
}
int entries = 0;

#if defined(__APPLE__)
const mach_timebase_info_data_t timebase = []
{
    mach_timebase_info_data_t t{};
    mach_timebase_info(&t);
    return t;
}();
#endif
} // namespace

std::uint64_t nowNs() noexcept
{
#if defined(__APPLE__)
    const std::uint64_t ticks = mach_absolute_time();
    if (timebase.numer == timebase.denom)
        return ticks;
    return ticks / timebase.denom * timebase.numer + ticks % timebase.denom * timebase.numer / timebase.denom;
#else
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000u + static_cast<std::uint64_t>(ts.tv_nsec);
#endif
}

Registrar::Registrar(const char* name, double budgetCycles, BenchFn fn, const char* file) noexcept
{
    if (find(name) != nullptr || entries >= kMaxEntries)
    {
        std::fprintf(stderr, "bench: duplicate bench name or registry full: %s (%s)\n", name, file);
        std::abort();
    }
    table()[static_cast<std::size_t>(entries++)] = Entry{name, budgetCycles, fn, file};
}

int count() noexcept
{
    return entries;
}
const Entry& entry(int index) noexcept
{
    return table()[static_cast<std::size_t>(index)];
}
const Entry* find(const char* name) noexcept
{
    for (int i = 0; i < entries; ++i)
        if (std::strcmp(table()[static_cast<std::size_t>(i)].name, name) == 0)
            return &table()[static_cast<std::size_t>(i)];
    return nullptr;
}

Result run(const Entry& e)
{
    core::rt::enableFlushToZero(); // the audio thread runs with FTZ; so do the benches
    State state;
    e.fn(state);
    return state.result(e.name, e.budgetCycles);
}

void format(const Result& r, char* buf, std::size_t size)
{
    if (! r.ran)
    {
        std::snprintf(buf, size, "BENCH %s budget=%.0f ERROR (the bench never called state.run)", r.name,
                      r.budgetCycles);
        return;
    }
    const double pct = r.budgetCycles > 0 ? 100.0 * r.medianCycles / r.budgetCycles : 0.0;
    std::snprintf(buf, size,
                  "BENCH %s budget=%.0f measured=%.1f (%.1f%% of budget) p95=%.1f min=%.1f cycles@1GHz batch=%llu "
                  "samples=%d %s",
                  r.name, r.budgetCycles, r.medianCycles, pct, r.p95Cycles, r.minCycles,
                  static_cast<unsigned long long>(r.batch), r.samples, r.over() ? "OVER" : "OK");
}
} // namespace bench
