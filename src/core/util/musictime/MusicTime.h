// Musical time shared by the engine (engine/transport, the sequencer, the scene switcher) and the UI (HOME's
// BAR:BEAT:STEP, the BPM/QUANTIZE panel): spec 02-features/sync-and-transport.md SYN-002/003/006, sequencer.md
// SEQ-002/003. Pure, header-only, RT-safe (no allocation, no libm), 4/4 only.
//
//   PPQ          transport position in quarter notes (double); one step = 1/16 note = 0.25 quarter (SEQ-002)
//   Position     BAR:BEAT:STEP, 1-based (SYN-006); a negative PPQ (the recording pre-count, SEQ-044) gives bar −N … −1
//   ppqPerFrame  quarters per sample frame at a tempo; the engine computes it once per block
//   frameOf      the first frame of a block at or after a PPQ — the one rounding rule every engine stage uses, so the
//                metronome, the sequencer and the scene switcher agree on the frame of an event to the sample
//   TapTempo     SYN-003: the average of the last 4 tap intervals, from the second tap; restarts after 2 s
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>

namespace core::musictime
{
constexpr double kQuartersPerStep = 0.25; // one step = 1/16 note (SEQ-002)
constexpr int kStepsPerBeat = 4, kBeatsPerBar = 4, kStepsPerBar = kStepsPerBeat * kBeatsPerBar;
constexpr double kQuartersPerBar = 4.0;
constexpr double kMinBpm = 20.0, kMaxBpm = 300.0; // SYN-002 range (ours), 0.1 BPM resolution

struct Position // 1-based, BAR:BEAT:STEP (SYN-006)
{
    int bar, beat, step;
    friend constexpr bool operator== (const Position&, const Position&) = default;
};

namespace detail
{
constexpr long long floorToInt(double x) noexcept
{
    const auto i = static_cast<long long>(x);
    return static_cast<double>(i) > x ? i - 1 : i;
}
constexpr long long floorDiv(long long a, long long b) noexcept
{
    const long long q = a / b;
    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}
} // namespace detail

// Steps are counted with a tolerance of 1e-6 step, so a position computed as 0.9999999999 quarters shows step 5.
// ppq < 0 (pre-count): bar −N … −1 counts the bars left, beat and step as usual (−8 quarters = −2:01:01).
constexpr Position toPosition(double ppqInPattern) noexcept
{
    const long long steps = detail::floorToInt(ppqInPattern / kQuartersPerStep + 1e-6);
    const long long bar = detail::floorDiv(steps, kStepsPerBar);
    const long long inBar = steps - bar * kStepsPerBar;
    return {static_cast<int>(bar >= 0 ? bar + 1 : bar), static_cast<int>(inBar / kStepsPerBeat) + 1,
            static_cast<int>(inBar % kStepsPerBeat) + 1};
}

constexpr double ppqPerFrame(double bpm, double sampleRate) noexcept { return bpm / (60.0 * sampleRate); }

// A BPM on the 0.1 grid inside 20.0–300.0 (SYN-002); NaN gives the default 120.
constexpr double clampBpm(double bpm) noexcept
{
    if (! (bpm == bpm))
        return 120.0;
    const double b = bpm < kMinBpm ? kMinBpm : (bpm > kMaxBpm ? kMaxBpm : bpm);
    return static_cast<double>(detail::floorToInt(b * 10.0 + 0.5)) / 10.0;
}

// Tolerance of frameOf in frames: far above the rounding error of a block's p0 after 24 h (≈ 1e-6 frame), far below
// one frame, so an event that lands exactly on a frame (a beat at 120 BPM, 44.1 kHz) is on that frame, not the next.
constexpr double kFrameEpsilon = 1e-4;

// The frame of an event at `ppq` in a block that starts at PPQ p0: the first frame f whose position p0 + f · ppqPerFrame
// reaches it, f = ⌈(ppq − p0) / ppqPerFrame − kFrameEpsilon⌉. Clamped to 0 for an event before p0 (it belongs to this
// block only when inBlock() says so) and capped at 1 << 30. ppqPerFrame must be > 0.
constexpr int frameOf(double ppq, double p0, double ppqPerFrame) noexcept
{
    const double x = (ppq - p0) / ppqPerFrame - kFrameEpsilon;
    if (! (x > 0.0))
        return 0;
    if (x >= 1073741824.0)
        return 1 << 30;
    const auto i = static_cast<int>(x);
    return static_cast<double>(i) < x ? i + 1 : i;
}

// True when an event at `ppq` sounds in the block of `frames` frames that starts at p0, i.e. its unclamped frame
// ⌈(ppq − p0) / ppqPerFrame − ε⌉ is in [0, frames). Consecutive blocks (p0' = p0 + frames · ppqPerFrame) partition the
// time line: every event belongs to exactly one block.
constexpr bool inBlock(double ppq, double p0, double ppqPerFrame, int frames) noexcept
{
    const double x = (ppq - p0) / ppqPerFrame - kFrameEpsilon;
    return x > -1.0 && x <= static_cast<double>(frames - 1);
}

// Calls fn(ppq, frame) for every multiple of `unit` (beats 1, steps 0.25, bars 4) that sounds in the block of `frames`
// frames starting at p0 (inBlock), in order. Nothing when ppqPerFrame ≤ 0 (the transport does not move).
template <typename Fn>
constexpr void forEachGridPoint(double p0, double ppqPerFrame, int frames, double unit, Fn&& fn) noexcept
{
    if (! (ppqPerFrame > 0.0) || ! (unit > 0.0))
        return;
    // A candidate at or just below the block's lower bound; the bound check skips it (rounding of lo / unit).
    long long m = detail::floorToInt((p0 - ppqPerFrame) / unit);
    for (int guard = 0; guard < frames + 4; ++guard, ++m)
    {
        const double k = static_cast<double>(m) * unit;
        const double x = (k - p0) / ppqPerFrame - kFrameEpsilon;
        if (x <= -1.0)
            continue;
        if (x > static_cast<double>(frames - 1))
            return;
        fn(k, frameOf(k, p0, ppqPerFrame));
    }
}

// "001:01:01" (bar three digits, SYN-006), a pre-count bar as "-02:01:01". Returns the length written. The fields are
// clamped to what a position can show (bar −999…99999, beat and step 0…99), so the text is at most 11 characters:
// a 16-byte buffer always holds it (and GCC can prove it: no -Wformat-truncation).
inline int formatPosition(const Position& p, char* buf, int size) noexcept
{
    const auto clamp = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
    const int beat = clamp(p.beat, 0, 99), step = clamp(p.step, 0, 99);
    if (p.bar < 0)
        return std::snprintf(buf, static_cast<std::size_t>(size), "-%02d:%02d:%02d", clamp(-p.bar, 0, 999), beat,
                             step);
    return std::snprintf(buf, static_cast<std::size_t>(size), "%03d:%02d:%02d", clamp(p.bar, 0, 99999), beat, step);
}

// SYN-003 tap tempo (message thread): from the second tap the BPM is 60000 / (mean of the last 4 intervals); a tap
// more than 2 s after the previous one starts a new series. No allocation. The caller rounds/clamps (clampBpm).
class TapTempo
{
public:
    static constexpr double kResetMs = 2000.0;
    static constexpr int kIntervals = 4;

    std::optional<double> tap(double nowMs) noexcept
    {
        if (count_ > 0 && (nowMs - last_ > kResetMs || nowMs < last_))
            reset();
        if (count_ > 0)
        {
            intervals_[static_cast<std::size_t>(next_)] = nowMs - last_;
            next_ = (next_ + 1) % kIntervals;
            filled_ = filled_ < kIntervals ? filled_ + 1 : kIntervals;
        }
        ++count_;
        last_ = nowMs;
        if (filled_ == 0)
            return std::nullopt;
        double sum = 0.0;
        for (int i = 0; i < filled_; ++i)
            sum += intervals_[static_cast<std::size_t>(i)];
        const double ms = sum / filled_;
        if (ms <= 0.0)
            return std::nullopt;
        return 60000.0 / ms;
    }
    void reset() noexcept { count_ = filled_ = next_ = 0; }
    int taps() const noexcept { return count_; } // taps in the current series

private:
    std::array<double, kIntervals> intervals_ {};
    int next_ = 0, filled_ = 0, count_ = 0;
    double last_ = 0.0;
};
} // namespace core::musictime
