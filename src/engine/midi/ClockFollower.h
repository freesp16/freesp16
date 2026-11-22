// External MIDI clock follower (spec 02-features/sync-and-transport.md SYN-011/012/013, NFR-004;
// OQ-18: the algorithm sits behind this class). Audio thread only, no allocation; doubles per tick / per
// block, never per sample.
//
// Input: the engine sample time of every incoming 24-PPQN tick (the tick's arrival mapped through the audio clock and
// scheduled at arrival + the fixed input latency — the same timeline as incoming notes, engine/midi/MidiInQueue.h).
//
// Estimator, our own (OQ-18; the spec's "second-order PLL, ~1 s time constant"): a least-squares line through the ticks
// of the last 2 s (≤ 256 ticks) — tick index → sample time. Its slope is the period P (tempo = 60·rate/(24·P)), its
// value at the last tick the filtered tick time. It is the loop filter a type-2 PLL approximates, without the PLL's
// settling transient: with ±1 ms of random jitter the tempo is within ±0.02 BPM (σ) after 2 s, and the phase within
// ≈ 0.1 ms. A sustained deviation of the newest ticks from the line (≥ 2 ms or 10 % of a tick, averaged over ~8
// ticks: a tempo change) restarts the window; a missing tick advances the index by the ticks it skipped.
//
// Display (SYN-012): the estimate rounded to 0.1 BPM with hysteresis — it changes only after the estimate has left
// ±0.07 BPM of the shown value for 500 ms, so a steady 160.0 shows 160.0 (the reference's hunting display is not
// reproduced). Nothing is shown before one quarter (24 ticks) of clock.
//
// Position (SYN-012 "phase locks to ticks"): setNextTickPosition(q) after Start (q = 0) or SPP / Continue; every tick
// then advances it by 1/24 quarter, and ppqAt(frame) extrapolates from the fitted last tick, so the transport can be
// steered onto it without drift. lost(now): no tick for more than 500 ms (SYN-013).
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
    static constexpr int kMaxTicks = 256;

    void prepare(double sampleRate) noexcept;
    void reset() noexcept; // forget the clock (source change, port change)

    // ---- audio thread ----
    void onTick(double sampleTime) noexcept;
    void onTick(std::int64_t sampleTime) noexcept { onTick(static_cast<double>(sampleTime)); }
    // Per block: the display hysteresis runs on block time.
    void updateDisplay(std::int64_t now) noexcept;
    void setNextTickPosition(double quarters) noexcept
    {
        nextTickPpq_ = quarters;
        hasPosition_ = true;
    }
    void clearPosition() noexcept { hasPosition_ = false; }

    // ---- reads ----
    double tempo() const noexcept;        // BPM of the estimate, 0 = none yet
    double displayTempo() const noexcept { return display_; } // 0.1 BPM grid, 0 = nothing to show yet
    bool lost(std::int64_t now) const noexcept;               // > 500 ms without a tick (or never a tick)
    bool locked() const noexcept { return count_ >= 2 && period_ > 0.0; }
    double framesPerTick() const noexcept { return period_; }
    double lastTickTime() const noexcept { return last_; }    // fitted
    std::int64_t ticks() const noexcept { return ticks_; }
    std::uint64_t reacquisitions() const noexcept { return reacquire_; }
    bool hasPosition() const noexcept { return hasPosition_ && lastHasPosition_ && locked(); }
    // The clock's position (quarters) at engine frame `frame`, extrapolated from the fitted last tick.
    double ppqAt(double frame) const noexcept { return lastPpq_ + (frame - last_) / (24.0 * period_); }

private:
    void restart(double t) noexcept;
    void fit() noexcept;

    double sampleRate_ = 44100.0;
    std::array<double, kMaxTicks> t_ {}; // tick times (frames), ring
    std::array<double, kMaxTicks> x_ {}; // tick index (ticks since the window's origin)
    int head_ = 0, count_ = 0;           // ring: oldest at head_
    double index_ = 0.0;                 // index of the newest tick
    double lastRaw_ = 0.0, last_ = 0.0, period_ = 0.0;
    double residualAvg_ = 0.0;
    std::int64_t ticks_ = 0;
    double display_ = 0.0;
    std::int64_t outsideSince_ = -1;
    double nextTickPpq_ = 0.0, lastPpq_ = 0.0;
    bool hasPosition_ = false, lastHasPosition_ = false;
    std::uint64_t reacquire_ = 0;
};
} // namespace engine::midi
