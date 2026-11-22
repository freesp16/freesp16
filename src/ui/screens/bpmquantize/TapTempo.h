// Interim tap tempo of the BPM/QUANTIZE panel (SYN-003): the average of the last 4 tap intervals, at least two
// taps, restarted after 2 s without a tap. Same contract as core::musictime::TapTempo, which replaces it. Message
// thread; no allocation.
#pragma once

#include <array>
#include <optional>

namespace ui::m2
{
class TapTempo
{
public:
    static constexpr double kResetMs = 2000;
    std::optional<double> tap(double nowMs) noexcept
    {
        if (count_ > 0 && nowMs - last_ > kResetMs)
            count_ = 0;
        if (count_ > 0)
        {
            intervals_[static_cast<std::size_t>(next_)] = nowMs - last_;
            next_ = (next_ + 1) % 4;
            filled_ = filled_ < 4 ? filled_ + 1 : 4;
        }
        else
            filled_ = next_ = 0;
        ++count_;
        last_ = nowMs;
        if (filled_ == 0)
            return std::nullopt;
        double sum = 0;
        for (int i = 0; i < filled_; ++i)
            sum += intervals_[static_cast<std::size_t>(i)];
        const double ms = sum / filled_;
        if (ms <= 0)
            return std::nullopt;
        return 60000.0 / ms;
    }
    int taps() const noexcept { return count_; }

private:
    std::array<double, 4> intervals_{};
    int next_ = 0, filled_ = 0, count_ = 0;
    double last_ = 0;
};
} // namespace ui::m2
