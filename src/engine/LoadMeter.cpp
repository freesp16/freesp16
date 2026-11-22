#include "engine/LoadMeter.h"

#include <algorithm>
#include <cmath>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#else
#include <time.h>
#endif

namespace engine
{
namespace
{
constexpr auto relaxed = std::memory_order_relaxed;
constexpr double windowSeconds = 10.0; // one statistics window; snapshot() covers this one + the previous one

#if defined(__APPLE__)
// Read at static-initialisation time, so the audio thread never runs a function-local static's guard.
const mach_timebase_info_data_t timebase = []
{
    mach_timebase_info_data_t t{};
    mach_timebase_info(&t);
    return t;
}();
#endif

// Nanoseconds of a raw monotonic clock: 1 ns = 1 cycle of the 1 GHz Cortex-A9 (the load normalisation).
inline std::uint64_t nowNs() noexcept
{
#if defined(__APPLE__)
    const std::uint64_t ticks = mach_absolute_time();
    if (timebase.numer == timebase.denom)
        return ticks;
    return ticks / timebase.denom * timebase.numer + ticks % timebase.denom * timebase.numer / timebase.denom;
#else
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts); // a syscall on the device's kernel (no ARM vDSO before Linux 4.1)
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000u + static_cast<std::uint64_t>(ts.tv_nsec);
#endif
}

// Single writer: a relaxed load + store is enough (no read-modify-write instruction needed).
template <typename T, typename V> inline void add(std::atomic<T>& a, V v) noexcept
{
    a.store(static_cast<T>(a.load(relaxed) + static_cast<T>(v)), relaxed);
}
} // namespace

static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "64-bit counters must be lock-free on the audio thread");

void LoadMeter::prepare(double sampleRate, int blockFrames)
{
    const double rate = sampleRate > 0 ? sampleRate : 44100.0;
    const int frames = blockFrames > 0 ? blockFrames : 64;
    blockFrames_ = static_cast<float>(frames);
    nsPerFrame_ = static_cast<float>(1.0e9 / rate);
    blockPeriodCycles_ = 1.0e9 * frames / rate;
    windowFrames_ = std::max<std::int64_t>(1, std::llround(windowSeconds * rate));
    startNs_ = 0;
    framesInWindow_ = 0;
    for (auto& bank : banks_)
    {
        for (auto& bin : bank.bins)
            bin.store(0, relaxed);
        bank.sum.store(0, relaxed);
        bank.count.store(0, relaxed);
        bank.max.store(0, relaxed);
    }
    current_.store(0, relaxed);
    blocks_.store(0, relaxed);
    overruns_.store(0, relaxed);
    generation_.store(generation_.load(relaxed) + 2, std::memory_order_release);
}

void LoadMeter::begin() noexcept
{
    startNs_ = nowNs();
}

void LoadMeter::end(int frames) noexcept
{
    record(nowNs() - startNs_, frames);
}

int LoadMeter::binOf(std::uint32_t cycles) noexcept
{
    if (cycles < static_cast<std::uint32_t>(kSub))
        return static_cast<int>(cycles);
    const int octave = 31 - __builtin_clz(cycles);
    return (octave - kSubBits + 1) * kSub + static_cast<int>((cycles >> (octave - kSubBits)) & (kSub - 1));
}

double LoadMeter::binValue(int bin) noexcept
{
    if (bin < kSub)
        return bin;
    const int octave = bin / kSub + kSubBits - 1;
    const int shift = octave - kSubBits;
    const double lower = static_cast<double>(static_cast<std::uint64_t>(kSub + bin % kSub) << shift);
    const double width = static_cast<double>(std::uint64_t{1} << shift);
    return lower + (width - 1.0) * 0.5;
}

void LoadMeter::record(std::uint64_t elapsedNs, int frames) noexcept
{
    if (frames <= 0)
        return;
    const float ns = static_cast<float>(std::min<std::uint64_t>(elapsedNs, 0xffffffffu));
    const float framesF = static_cast<float>(frames);
    float cyclesF = ns;
    if (framesF != blockFrames_)
        cyclesF *= blockFrames_ / framesF; // cycles per engine block
    const std::uint32_t cycles =
        cyclesF >= static_cast<float>(kMaxValue) ? kMaxValue : static_cast<std::uint32_t>(cyclesF);

    Bank& bank = banks_[current_.load(relaxed)];
    add(bank.bins[static_cast<std::size_t>(binOf(cycles))], 1);
    add(bank.sum, cycles);
    add(bank.count, 1);
    if (cycles > bank.max.load(relaxed))
        bank.max.store(cycles, relaxed);
    add(blocks_, 1);
    if (ns > framesF * nsPerFrame_)
        add(overruns_, 1);

    framesInWindow_ += frames;
    if (framesInWindow_ >= windowFrames_)
        rotate();
}

void LoadMeter::rotate() noexcept
{
    // Seqlock writer: generation odd while the older bank is cleared and made current. Once per 10 s, ~736 stores.
    const std::uint32_t next = current_.load(relaxed) ^ 1u;
    const std::uint32_t g = generation_.load(relaxed);
    generation_.store(g + 1, relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    Bank& bank = banks_[next];
    for (auto& bin : bank.bins)
        bin.store(0, relaxed);
    bank.sum.store(0, relaxed);
    bank.count.store(0, relaxed);
    bank.max.store(0, relaxed);
    current_.store(next, relaxed);
    generation_.store(g + 2, std::memory_order_release);
    framesInWindow_ = 0;
}

LoadMeter::Stats LoadMeter::snapshot() const noexcept
{
    Stats s{};
    std::array<std::uint32_t, kBins> bins{};
    std::uint64_t sum = 0, count = 0;
    std::uint32_t max = 0;
    // Seqlock reader: retry when a rotation (every 10 s) overlapped the read; bounded, so never blocks.
    for (int attempt = 0; attempt < 16; ++attempt)
    {
        const std::uint32_t g1 = generation_.load(std::memory_order_acquire);
        if ((g1 & 1u) != 0)
            continue;
        sum = count = 0;
        max = 0;
        for (std::size_t i = 0; i < bins.size(); ++i)
            bins[i] = banks_[0].bins[i].load(relaxed) + banks_[1].bins[i].load(relaxed);
        for (const auto& bank : banks_)
        {
            sum += bank.sum.load(relaxed);
            count += bank.count.load(relaxed);
            max = std::max(max, bank.max.load(relaxed));
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (generation_.load(relaxed) == g1)
            break;
    }
    s.blocks = blocks_.load(relaxed);
    s.overruns = overruns_.load(relaxed);
    if (count == 0)
        return s;

    std::uint64_t total = 0;
    for (auto n : bins)
        total += n;
    const std::uint64_t rank = (total * 95 + 99) / 100; // ceil(0.95 n): the p95 sample, 1-based
    std::uint64_t seen = 0;
    int p95Bin = kBins - 1;
    for (int i = 0; i < kBins; ++i)
    {
        seen += bins[static_cast<std::size_t>(i)];
        if (seen >= rank && seen > 0)
        {
            p95Bin = i;
            break;
        }
    }
    s.avgCycles = static_cast<double>(sum) / static_cast<double>(count);
    s.maxCycles = max;
    s.p95Cycles = std::min(binValue(p95Bin), s.maxCycles);
    s.loadPercent = 100.0 * s.avgCycles / blockPeriodCycles_;
    return s;
}
} // namespace engine
