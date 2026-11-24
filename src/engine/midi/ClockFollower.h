// External MIDI clock follower (spec 02-features/sync-and-transport.md SYN-011/012/013, NFR-004;
// OQ-18: the algorithm sits behind this class). Audio thread only, no allocation; doubles per tick / per
// block, never per sample.
//
// Input: the engine sample time of every incoming 24-PPQN tick (the tick's arrival mapped through the audio clock and
// scheduled at arrival + the fixed input latency — the same timeline as incoming notes, engine/midi/MidiInQueue.h).
//
// Counting: every F8 is exactly one tick. MIDI does not lose bytes, but it delivers them late and in bursts (a DAW
// renders its clock with its audio blocks, USB polls every few ms, the master's scheduler hiccups), so a tick is never
// inferred from a gap: the tick index is the count of F8s, and the song position follows it exactly
// (position = the position given at Start / Continue / SPP + ticks since then / 24). Timing noise moves the estimate
// of *when* a tick was, never *which* tick it was — the position cannot drift.
//
// Estimator (ours, OQ-18; the spec's "second-order PLL, ~1 s time constant"): a least-squares line through the ticks of
// the last 2 s (≤ 256 ticks) — tick index → sample time. Its slope is the period P (tempo = 60·rate/(24·P)), its value
// at the newest tick the filtered tick time: the loop filter a type-2 PLL approximates, without the PLL's settling
// transient. ±1 ms random jitter: tempo within ±0.02 BPM (σ) after 2 s, phase within ≈ 0.1 ms; burst jitter of 0…16 ms
// is averaged the same way (its mean is a constant latency). A tempo change is a *sustained* deviation from the line:
// the residual averaged over ~8 ticks beyond max(2 ms, 3 σ of the measured jitter) — a single late tick, however late,
// is jitter. The line is then refitted on the newest ticks only (the index and the position run on).
//
// Display (SYN-012): the estimate rounded to 0.1 BPM with hysteresis — it changes only after the estimate has left
// ±0.07 BPM of the shown value for 500 ms, so a steady 160.0 shows 160.0. Nothing is shown before one quarter
// (24 ticks) of clock.
//
// Position (SYN-012 "phase locks to ticks"): setNextTickPosition(q) after Start (q = 0), Continue or SPP; without one
// (sync switched on mid-song while the unit plays, the clock back after a loss) the transport's position is adopted onto the
// tick grid (adoptPosition). ppqAt(frame) extrapolates from the fitted newest tick, so the transport is steered onto
// the clock without drift. lost(now): no tick for more than 500 ms (SYN-013).
#pragma once

#include <array>
#include <cstdint>

namespace engine::midi
{
class ClockFollower
{
public:
    static constexpr double kLostSeconds = 0.5;     // SYN-013
    static constexpr double kHysteresisBpm = 0.07;  // display
    static constexpr double kHysteresisSeconds = 0.5;
    static constexpr double kWindowSeconds = 2.0;
    static constexpr int kMaxTicks = 256;           // power of two
    static constexpr int kRefitTicks = 4;           // the newest ticks kept when the tempo changed
    static constexpr double kMinBpm = 10.0, kMaxBpm = 999.0; // a fitted period outside is noise (two ticks in a burst)

    void prepare(double sampleRate) noexcept;
    void reset() noexcept; // forget the clock (source change, port change)

    // ---- audio thread ----
    void onTick(double sampleTime) noexcept;
    void onTick(std::int64_t sampleTime) noexcept { onTick(static_cast<double>(sampleTime)); }
    // Per block: the display hysteresis runs on block time.
    void updateDisplay(std::int64_t now) noexcept;
    // The position (quarters) of the next tick: Start (0), Continue (where the transport paused), SPP.
    void setNextTickPosition(double quarters) noexcept
    {
        pendingPpq_ = quarters;
        pending_ = true;
    }
    // Forget the position (Stop, or the transport stopped by the unit itself); a pending one is kept unless `all`.
    void clearPosition(bool all = true) noexcept
    {
        hasPosition_ = false;
        if (all)
            pending_ = false;
    }
    // No Start / SPP told us where the clock is: the newest tick is at `quarters` (the transport's own position there,
    // rounded to the tick grid), so from now on the transport keeps its bars and is phase-locked to the ticks.
    void adoptPosition(double quarters) noexcept;

    // ---- reads ----
    double tempo() const noexcept;        // BPM of the estimate, 0 = none yet
    double displayTempo() const noexcept { return display_; } // 0.1 BPM grid, 0 = nothing to show yet
    bool lost(std::int64_t now) const noexcept;               // > 500 ms without a tick (or never a tick)
    bool locked() const noexcept { return count_ >= 2 && period_ > 0.0; }
    double framesPerTick() const noexcept { return period_; }
    double lastTickTime() const noexcept { return last_; }    // fitted
    std::int64_t ticks() const noexcept { return ticks_; }
    std::uint64_t reacquisitions() const noexcept { return reacquire_; }
    double jitterFrames() const noexcept;                     // RMS deviation of the ticks from the line
    bool hasPosition() const noexcept { return hasPosition_ && locked(); }
    bool positionPending() const noexcept { return pending_; }
    // The clock's position (quarters) of the newest tick, and at engine frame `frame` (extrapolated from the fitted
    // newest tick).
    double lastTickPpq() const noexcept { return posBase_ + static_cast<double>(index_ - posIndex_) / 24.0; }
    double ppqAt(double frame) const noexcept { return lastTickPpq() + (frame - last_) / (24.0 * period_); }

private:
    void restart(double t) noexcept;
    void fit() noexcept;
    std::size_t slot(int i) const noexcept { return static_cast<std::size_t>((head_ + i) & (kMaxTicks - 1)); }

    double sampleRate_ = 44100.0;
    std::array<double, kMaxTicks> t_ {}; // tick times (frames), ring, oldest at head_
    std::array<double, kMaxTicks> x_ {}; // tick index relative to origin_
    int head_ = 0, count_ = 0;
    std::int64_t index_ = 0, origin_ = 0; // newest tick's index (= ticks counted); index of the fit's x = 0
    double lastRaw_ = 0.0, last_ = 0.0, period_ = 0.0;
    double residualAvg_ = 0.0, residualVar_ = 0.0;
    std::int64_t ticks_ = 0;
    double display_ = 0.0;
    std::int64_t outsideSince_ = -1;
    double posBase_ = 0.0, pendingPpq_ = 0.0; // position of tick posIndex_; of the next tick when pending_
    std::int64_t posIndex_ = 0;
    bool hasPosition_ = false, pending_ = false;
    std::uint64_t reacquire_ = 0;
};
} // namespace engine::midi
