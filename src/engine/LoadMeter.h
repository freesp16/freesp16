// Engine load meter (the deadline watchdog): the time the audio callback spends
// per engine block, against the block period. Feeds the app's `LOAD` log line (cycles/block avg p95 max, overruns).
//
//   prepare(sampleRate, blockFrames)  message thread, audio stopped: sets the block period, clears every statistic
//   begin() / end(frames)             audio thread, around the work of `frames` frames (one engine block or one whole
//                                     device period: the time is scaled to cycles per `blockFrames` block)
//   snapshot()                        any one other thread (message thread), lock-free and wait-free for the audio side
//
// Cycles are normalised to the Cortex-A9's 1 GHz: one cycle = one nanosecond of CLOCK_MONOTONIC_RAW (device, Linux) or
// mach_absolute_time (macOS host). Budget: ≤ 700 k cycles per 64-frame block at 44.1 kHz (≤ 50 % of one core).
//
// Statistics (Stats): avgCycles, p95Cycles and maxCycles cover the recent window — the window being filled plus the
// previous complete one, each 10 s of audio, so 10–20 s of history once running; loadPercent = avgCycles /
// block period; blocks (end() calls) and overruns (calls slower than the real time of their frames) count since
// prepare(). p95 comes from a fixed log-linear histogram (32 sub-bins per octave: ≤ 3.2 % error); nothing allocates or
// locks. Cost on the audio thread: two clock reads + a few relaxed stores per block (bench: load_meter_begin_end).
#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace engine
{
class LoadMeter
{
public:
    void prepare(double sampleRate, int blockFrames);
    void begin() noexcept;
    void end(int frames) noexcept;

    struct Stats
    {
        double avgCycles, p95Cycles, maxCycles, loadPercent;
        std::uint64_t blocks, overruns;
    };
    Stats snapshot() const noexcept; // message thread, lock-free

private:
    friend struct LoadMeterTestAccess; // tests/unit/perf: feeds exact durations through record()

    static constexpr int kSubBits = 5; // 32 linear sub-bins per octave
    static constexpr int kSub = 1 << kSubBits;
    static constexpr int kMaxOctave = 26;                            // values up to 2^27 - 1 cycles (134 ms)
    static constexpr int kBins = (kMaxOctave - kSubBits + 2) * kSub; // 736
    static constexpr std::uint32_t kMaxValue = (1u << (kMaxOctave + 1)) - 1;

    static int binOf(std::uint32_t cycles) noexcept;
    static double binValue(int bin) noexcept; // representative value (midpoint) of a bin

    void record(std::uint64_t elapsedNs, int frames) noexcept;
    void rotate() noexcept;

    struct Bank
    {
        std::array<std::atomic<std::uint32_t>, kBins> bins;
        std::atomic<std::uint64_t> sum;
        std::atomic<std::uint32_t> count;
        std::atomic<std::uint32_t> max;
    };

    // Set in prepare() (audio stopped), read-only afterwards.
    float blockFrames_ = 64.0f;
    float nsPerFrame_ = 1.0e9f / 44100.0f;
    double blockPeriodCycles_ = 64.0e9 / 44100.0;
    std::int64_t windowFrames_ = 441000;

    // Audio thread only.
    std::uint64_t startNs_ = 0;
    std::int64_t framesInWindow_ = 0;

    // Written by the audio thread (single writer: relaxed load + store), read by snapshot().
    std::array<Bank, 2> banks_{};
    std::atomic<std::uint32_t> current_{0};    // bank being filled
    std::atomic<std::uint32_t> generation_{0}; // odd while rotate() clears a bank (seqlock for snapshot)
    std::atomic<std::uint64_t> blocks_{0};
    std::atomic<std::uint64_t> overruns_{0};
};
} // namespace engine
