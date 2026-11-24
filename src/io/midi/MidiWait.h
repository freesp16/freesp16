#pragma once

#include "core/util/midi/MidiWire.h"

#include <cerrno>
#include <time.h>

namespace io::midi
{
// Sender and virtual-port threads use the same clock as their MIDI timestamps.
inline void waitUntilMonotonicNs(std::int64_t deadline) noexcept
{
    constexpr std::int64_t nsPerSecond = 1000000000;
#if defined(__APPLE__)
    // Darwin provides a relative sleep. Recompute the interval after an interruption to keep the original deadline.
    for (auto remaining = deadline - core::midi::monotonicNs(); remaining > 0;
         remaining = deadline - core::midi::monotonicNs())
    {
        const timespec delay{static_cast<time_t>(remaining / nsPerSecond), static_cast<long>(remaining % nsPerSecond)};
        if (nanosleep(&delay, nullptr) == 0 || errno != EINTR)
            return;
    }
#else
    const timespec time{static_cast<time_t>(deadline / nsPerSecond), static_cast<long>(deadline % nsPerSecond)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &time, nullptr) == EINTR)
    {
    }
#endif
}
} // namespace io::midi
