// Time source for message-thread code (frame loop, coalescing windows EDT-U2, toasts, animations UID-065). Injected
// so tests drive time explicitly (tests/fakes/FakeClock.h). The audio thread never uses a Clock: engine time is the
// sample/block count.
#pragma once

#include <chrono>

namespace core
{
class Clock
{
public:
    virtual ~Clock() = default;
    virtual double nowMs() const noexcept = 0; // monotonic milliseconds, arbitrary origin
};

class SteadyClock final : public Clock
{
public:
    double nowMs() const noexcept override
    {
        using namespace std::chrono;
        return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
    }
};
} // namespace core
