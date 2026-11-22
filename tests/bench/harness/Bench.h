// Benchmark harness. One benchmark:
//
//   #include "tests/bench/harness/Bench.h"
//   BENCH(voice_render, 6000)                       // name (an identifier), budget in A9 cycles per iteration
//   {
//       Voice v;  v.prepare(...);                   // set-up: not timed
//       state.run([&] { v.render(out, 64); bench::doNotOptimize(out); });   // timed: one iteration = one block
//   }
//
// Cycles are normalised to the Cortex-A9's 1 GHz: one cycle = one nanosecond of mach_absolute_time (macOS host) or
// CLOCK_MONOTONIC_RAW (Linux, device). State::run calibrates a batch of iterations that lasts >= 100 µs (so clock
// resolution and syscall cost vanish), then times up to 101 batches (2 s at most) and keeps the per-iteration time of
// each: measured = median, also p95 and min. Every bench prints one line
//   BENCH <name> budget=<cycles> measured=<cycles> (<n>% of budget) p95=<cycles> min=<cycles> batch=<n> samples=<n>
//   OK|OVER
//
// Where it runs:
//   host   tests/bench/**/*.cpp → <prefix>_bench (Catch2, Release only): every BENCH is a test case, label `bench`
//          (`ctest --preset host-release -L bench`). It fails when measured > budget, and writes
//          <build>/artifacts/bench/<name>.json for the regression check (tests/bench/harness/check_regression.py, CI).
//   device scripts/emulator-bench.sh <bench>: the harness + the chosen bench files built standalone (BENCH_STANDALONE,
//          tests/bench/harness/device/) and run as the app in the emulator; lines carry the app's `[sp16-mod]` log
//          tag. Emulator timings are relative only (qemu-arm user mode on a faster host).
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

#if ! defined(BENCH_STANDALONE)
#include <catch2/catch_test_macros.hpp>
#endif

namespace bench
{
// Raw monotonic clock in nanoseconds = A9 cycles at 1 GHz.
std::uint64_t nowNs() noexcept;

// Keep a value (or what a pointer points to) alive and observable, so the optimiser cannot drop the work.
template <typename T> inline void doNotOptimize(const T& value) noexcept
{
    asm volatile("" : : "r,m"(value) : "memory");
}
inline void clobberMemory() noexcept
{
    asm volatile("" : : : "memory");
}

struct Result
{
    const char* name = "";
    double budgetCycles = 0;
    double medianCycles = 0, p95Cycles = 0, minCycles = 0; // per iteration
    std::uint64_t batch = 0;                               // iterations per timed sample
    int samples = 0;
    bool ran = false; // the bench called State::run
    bool over() const noexcept { return medianCycles > budgetCycles; }
};

class State
{
public:
    static constexpr int kMaxSamples = 101;

    State(double targetBatchNs = 100000.0, int samples = kMaxSamples, double maxSeconds = 2.0) noexcept
        : targetBatchNs_(targetBatchNs), wanted_(std::clamp(samples, 1, kMaxSamples)), maxNs_(maxSeconds * 1.0e9)
    {
    }

    // Times `body` (one iteration). Call it once per bench, after the set-up.
    template <typename Body> void run(Body&& body)
    {
        // Warm-up, untimed: caches, branch predictors — and in the emulator qemu's first translation of the code, which
        // would otherwise make the very first batch look long enough.
        const std::uint64_t warm = nowNs();
        for (int i = 0; i < 1000 && (i < 16 || static_cast<double>(nowNs() - warm) < 2.0 * targetBatchNs_); ++i)
            body();
        // Calibration: double until one batch lasts targetBatchNs_; each size is timed twice and the faster counts
        // (the first pass through this loop is itself new code to qemu).
        std::uint64_t batch = 1;
        for (;;)
        {
            std::uint64_t dt = ~std::uint64_t{0};
            for (int pass = 0; pass < 2; ++pass)
            {
                const std::uint64_t t0 = nowNs();
                for (std::uint64_t i = 0; i < batch; ++i)
                    body();
                dt = std::min(dt, nowNs() - t0);
            }
            if (static_cast<double>(dt) >= targetBatchNs_ || batch >= (std::uint64_t{1} << 26))
                break;
            batch *= 2;
        }
        const std::uint64_t start = nowNs();
        count_ = 0;
        for (int s = 0; s < wanted_; ++s)
        {
            const std::uint64_t t0 = nowNs();
            for (std::uint64_t i = 0; i < batch; ++i)
                body();
            const std::uint64_t t1 = nowNs();
            samples_[static_cast<std::size_t>(count_++)] = static_cast<double>(t1 - t0) / static_cast<double>(batch);
            if (count_ >= 5 && static_cast<double>(t1 - start) > maxNs_)
                break;
        }
        batch_ = batch;
    }

    bool ran() const noexcept { return count_ > 0; }
    // Statistics of the samples (sorts them).
    Result result(const char* name, double budgetCycles) noexcept
    {
        Result r;
        r.name = name;
        r.budgetCycles = budgetCycles;
        r.ran = count_ > 0;
        if (! r.ran)
            return r;
        auto* first = samples_.data();
        std::sort(first, first + count_);
        r.minCycles = first[0];
        r.medianCycles = first[count_ / 2];
        r.p95Cycles = first[std::min(count_ - 1, (count_ * 95 + 99) / 100 - 1)];
        r.batch = batch_;
        r.samples = count_;
        return r;
    }

private:
    double targetBatchNs_;
    int wanted_;
    double maxNs_;
    std::array<double, kMaxSamples> samples_{};
    int count_ = 0;
    std::uint64_t batch_ = 0;
};

using BenchFn = void (*)(State&);
struct Entry
{
    const char* name;
    double budgetCycles;
    BenchFn fn;
    const char* file;
};

// Static registration (BENCH). A fixed table: no allocation during static initialisation.
struct Registrar
{
    Registrar(const char* name, double budgetCycles, BenchFn fn, const char* file) noexcept;
};
int count() noexcept;
const Entry& entry(int index) noexcept;
const Entry* find(const char* name) noexcept;

// Runs one registered bench (flush-to-zero on, like the audio thread) and returns its statistics.
Result run(const Entry& e);
// The one-line report (without trailing newline) into buf.
void format(const Result& r, char* buf, std::size_t size);

#if ! defined(BENCH_STANDALONE)
// Host benches that time themselves (e.g. a whole Catch2 test case measuring several things): report one measured
// quantity the same way as a BENCH — the standard line + <artifacts>/bench/<name>.json for the regression check.
// Gating stays with the caller's own CHECKs. Times in A9 cycles at 1 GHz (= ns; ms × 1e6). p95 < 0: same as measured.
void record(const char* name, double budgetCycles, double measuredCycles, double p95Cycles = -1.0);
inline double msToCycles(double ms) noexcept
{
    return ms * 1.0e6;
}
inline double usToCycles(double us) noexcept
{
    return us * 1.0e3;
}
#endif

namespace detail
{
void catchRun(const char* name); // host: run, print, write the JSON result, CHECK the budget (CatchRunner.cpp)
}
} // namespace bench

#if defined(BENCH_STANDALONE)
#define BENCH_DETAIL_CASE(name)
#else
#define BENCH_DETAIL_CASE(name)                                                                                        \
    TEST_CASE(#name, "[bench]")                                                                                        \
    {                                                                                                                  \
        ::bench::detail::catchRun(#name);                                                                              \
    }
#endif

#define BENCH(name, budgetCycles)                                                                                      \
    static void benchBody_##name(::bench::State& state);                                                               \
    static const ::bench::Registrar benchRegistrar_##name{#name, static_cast<double>(budgetCycles), &benchBody_##name, \
                                                          __FILE__};                                                   \
    BENCH_DETAIL_CASE(name)                                                                                            \
    static void benchBody_##name([[maybe_unused]] ::bench::State& state)
