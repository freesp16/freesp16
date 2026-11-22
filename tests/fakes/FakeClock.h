// FakeClock: a core::Clock tests advance explicitly (coalescing windows, animations, toasts).
#pragma once

#include "core/util/Clock.h"

namespace fakes
{
class FakeClock final : public core::Clock
{
public:
    explicit FakeClock(double startMs = 0) : now_(startMs) {}
    double nowMs() const noexcept override { return now_; }
    void advance(double ms) noexcept { now_ += ms; }
    void set(double ms) noexcept { now_ = ms; }

private:
    double now_;
};
} // namespace fakes
