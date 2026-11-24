#include "engine/midi/ClockFollower.h"

#include <cmath>

namespace engine::midi
{
void ClockFollower::prepare(double sampleRate) noexcept
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    reset();
}

void ClockFollower::reset() noexcept
{
    head_ = count_ = run_ = 0;
    index_ = origin_ = 0;
    lastRaw_ = last_ = period_ = 0.0;
    residualAvg_ = 0.0;
    residualVar_ = 1.0e-6 * sampleRate_ * sampleRate_; // (1 ms)²: no jitter measured yet
    ticks_ = 0;
    display_ = 0.0;
    outsideSince_ = -1;
    posBase_ = pendingPpq_ = 0.0;
    posIndex_ = 0;
    hasPosition_ = pending_ = false;
    reacquire_ = 0;
}

void ClockFollower::restart(double t) noexcept
{
    // A new line from this tick on (first tick, or after a gap): the index and the position run on.
    head_ = 0;
    count_ = 1;
    run_ = 1;
    origin_ = index_;
    t_[0] = t;
    x_[0] = 0.0;
    residualAvg_ = 0.0;
    lastRaw_ = last_ = t;
    period_ = 0.0;
}

void ClockFollower::adoptPosition(double quarters) noexcept
{
    posBase_ = std::nearbyint(quarters * 24.0) / 24.0;
    posIndex_ = index_;
    hasPosition_ = true;
}

void ClockFollower::fit() noexcept
{
    // Least squares through (x_i, t_i), centred (doubles: sums of a few hundred values, no cancellation trouble).
    double mx = 0.0, mt = 0.0;
    for (int i = 0; i < count_; ++i)
    {
        mx += x_[slot(i)];
        mt += t_[slot(i)];
    }
    mx /= count_;
    mt /= count_;
    double sxx = 0.0, sxt = 0.0;
    for (int i = 0; i < count_; ++i)
    {
        const double dx = x_[slot(i)] - mx;
        sxx += dx * dx;
        sxt += dx * (t_[slot(i)] - mt);
    }
    const double minPeriod = 60.0 * sampleRate_ / (24.0 * kMaxBpm), maxPeriod = 60.0 * sampleRate_ / (24.0 * kMinBpm);
    if (sxx > 0.0)
    {
        const double p = sxt / sxx;
        if (p >= minPeriod && p <= maxPeriod)
            period_ = p;
    }
    if (period_ > 0.0)
        last_ = mt + period_ * (static_cast<double>(index_ - origin_) - mx);
    else
        last_ = lastRaw_;
}

void ClockFollower::onTick(double t) noexcept
{
    ++ticks_;
    ++index_;
    const double gap = kLostSeconds * sampleRate_;
    const bool fresh = count_ == 0 || t - lastRaw_ > gap || t < lastRaw_ - gap;
    if (fresh && count_ > 0)
        hasPosition_ = false; // ticks went missing: the count no longer says where the clock is (re-adopted)
    if (pending_)
    {
        posBase_ = pendingPpq_;
        posIndex_ = index_;
        hasPosition_ = true;
        pending_ = false;
    }
    if (fresh)
    {
        restart(t); // first tick, after a pause of the clock, or the timeline jumped (device restart)
        return;
    }
    if (t < lastRaw_)
        t = lastRaw_; // never before the previous tick (two ticks in one read, an anchor re-sync)
    if (period_ > 0.0)
    {
        // Deviation from the line; only a sustained one is a tempo change (a late tick is jitter: it is counted once).
        const double r = t - (last_ + period_);
        residualAvg_ += (r - residualAvg_) * 0.125;
        const double limit = std::fmax(0.002 * sampleRate_, 3.0 * std::sqrt(residualVar_));
        if (std::fabs(residualAvg_) > limit)
        {
            ++reacquire_;
            while (count_ > kRefitTicks - 1)
            {
                head_ = (head_ + 1) & (kMaxTicks - 1);
                --count_;
            }
            residualAvg_ = 0.0;
        }
        else
        {
            const double rc = std::fmin(std::fabs(r), 0.5 * period_); // one wild tick does not blow up the jitter
            residualVar_ += (rc * rc - residualVar_) * (1.0 / 32.0);
        }
    }
    // Append; drop what is older than the window (or beyond the ring).
    if (count_ == kMaxTicks)
    {
        head_ = (head_ + 1) & (kMaxTicks - 1);
        --count_;
    }
    t_[slot(count_)] = t;
    x_[slot(count_)] = static_cast<double>(index_ - origin_);
    ++count_;
    while (count_ > 2 && t - t_[slot(0)] > kWindowSeconds * sampleRate_)
    {
        head_ = (head_ + 1) & (kMaxTicks - 1);
        --count_;
    }
    lastRaw_ = t;
    if (run_ < kLockTicks)
        ++run_;
    fit();
}

double ClockFollower::tempo() const noexcept
{
    return locked() ? 60.0 * sampleRate_ / (24.0 * period_) : 0.0;
}

double ClockFollower::jitterFrames() const noexcept { return std::sqrt(residualVar_); }

bool ClockFollower::lost(std::int64_t now) const noexcept
{
    return count_ == 0 || static_cast<double>(now) - lastRaw_ > kLostSeconds * sampleRate_;
}

void ClockFollower::updateDisplay(std::int64_t now) noexcept
{
    const double est = tempo();
    if (est <= 0.0 || ticks_ < 24)
    {
        if (ticks_ == 0)
            display_ = 0.0;
        outsideSince_ = -1;
        return;
    }
    const double rounded = std::nearbyint(est * 10.0) / 10.0;
    if (display_ <= 0.0)
    {
        display_ = rounded;
        outsideSince_ = -1;
        return;
    }
    if (std::fabs(est - display_) <= kHysteresisBpm)
    {
        outsideSince_ = -1;
        return;
    }
    if (outsideSince_ < 0)
        outsideSince_ = now;
    else if (static_cast<double>(now - outsideSince_) >= kHysteresisSeconds * sampleRate_)
    {
        display_ = rounded;
        outsideSince_ = -1;
    }
}
} // namespace engine::midi
