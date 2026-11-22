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
    head_ = count_ = 0;
    index_ = 0.0;
    lastRaw_ = last_ = period_ = 0.0;
    residualAvg_ = 0.0;
    ticks_ = 0;
    display_ = 0.0;
    outsideSince_ = -1;
    lastPpq_ = nextTickPpq_ = 0.0;
    hasPosition_ = lastHasPosition_ = false;
}

void ClockFollower::restart(double t) noexcept
{
    head_ = 0;
    count_ = 1;
    index_ = 0.0;
    t_[0] = t;
    x_[0] = 0.0;
    residualAvg_ = 0.0;
}

void ClockFollower::fit() noexcept
{
    // Least squares through (x_i, t_i), centred (doubles: sums of a few hundred values, no cancellation trouble).
    double mx = 0.0, mt = 0.0;
    for (int i = 0; i < count_; ++i)
    {
        const int k = (head_ + i) & (kMaxTicks - 1);
        mx += x_[static_cast<std::size_t>(k)];
        mt += t_[static_cast<std::size_t>(k)];
    }
    mx /= count_;
    mt /= count_;
    double sxx = 0.0, sxt = 0.0;
    for (int i = 0; i < count_; ++i)
    {
        const int k = (head_ + i) & (kMaxTicks - 1);
        const double dx = x_[static_cast<std::size_t>(k)] - mx;
        sxx += dx * dx;
        sxt += dx * (t_[static_cast<std::size_t>(k)] - mt);
    }
    if (sxx > 0.0)
    {
        const double p = sxt / sxx;
        if (p > 0.0)
            period_ = p;
    }
    last_ = mt + period_ * (index_ - mx);
}

void ClockFollower::onTick(double t) noexcept
{
    lastHasPosition_ = hasPosition_;
    const double maxPeriod = 60.0 * sampleRate_ / (24.0 * 10.0); // 10 BPM
    ++ticks_;
    if (count_ > 0 && t <= lastRaw_ && t > lastRaw_ - kLostSeconds * sampleRate_)
    {
        // Same stamp as the previous tick (two ticks in one read of the port): count it, keep the line.
        index_ += 1.0;
        if (count_ > 0 && period_ > 0.0)
            last_ += period_;
        lastPpq_ = nextTickPpq_;
        if (hasPosition_)
            nextTickPpq_ += 1.0 / 24.0;
        return;
    }
    if (count_ == 0 || t - lastRaw_ > kLostSeconds * sampleRate_ || t < lastRaw_ ||
        (period_ <= 0.0 && t - lastRaw_ > maxPeriod))
    {
        // First tick, or after a gap: nothing to estimate yet.
        restart(t);
        lastRaw_ = last_ = t;
        period_ = 0.0;
        lastPpq_ = nextTickPpq_;
        if (hasPosition_)
            nextTickPpq_ += 1.0 / 24.0;
        return;
    }
    // Ticks since the previous one (a lost tick counts as skipped, the position follows).
    double step = 1.0;
    if (period_ > 0.0)
    {
        const double k = std::nearbyint((t - lastRaw_) / period_);
        step = k < 1.0 ? 1.0 : k;
        // Deviation of this tick from the line; a sustained one is a tempo change: restart the window.
        const double predicted = last_ + period_ * step;
        const double r = t - predicted;
        residualAvg_ += (r - residualAvg_) * 0.125;
        const double limit = std::fmax(0.1 * period_, 0.002 * sampleRate_);
        if (std::fabs(residualAvg_) > limit || std::fabs(r) > 0.5 * period_)
        {
            ++reacquire_;
            const double p = (t - lastRaw_) / step;
            if (hasPosition_)
                nextTickPpq_ += (step - 1.0) / 24.0;
            restart(lastRaw_);
            index_ = step;
            t_[1] = t;
            x_[1] = step;
            count_ = 2;
            period_ = p;
            last_ = t;
            lastRaw_ = t;
            lastPpq_ = nextTickPpq_;
            if (hasPosition_)
                nextTickPpq_ += 1.0 / 24.0;
            return;
        }
    }
    if (hasPosition_)
        nextTickPpq_ += (step - 1.0) / 24.0;
    index_ += step;
    // Append; drop what is older than the window (or beyond the ring).
    if (count_ == kMaxTicks)
    {
        head_ = (head_ + 1) & (kMaxTicks - 1);
        --count_;
    }
    const int k = (head_ + count_) & (kMaxTicks - 1);
    t_[static_cast<std::size_t>(k)] = t;
    x_[static_cast<std::size_t>(k)] = index_;
    ++count_;
    while (count_ > 2 && t - t_[static_cast<std::size_t>(head_)] > kWindowSeconds * sampleRate_)
    {
        head_ = (head_ + 1) & (kMaxTicks - 1);
        --count_;
    }
    lastRaw_ = t;
    fit();
    lastPpq_ = nextTickPpq_;
    if (hasPosition_)
        nextTickPpq_ += 1.0 / 24.0;
}

double ClockFollower::tempo() const noexcept
{
    return locked() ? 60.0 * sampleRate_ / (24.0 * period_) : 0.0;
}

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
