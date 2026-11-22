#include "core/util/midi/MidiWire.h"

#include <time.h>

namespace core::midi
{
namespace
{
std::atomic<MidiHub*>& live() noexcept
{
    static std::atomic<MidiHub*> hub {nullptr};
    return hub;
}
} // namespace

MidiHub* liveHub() noexcept { return live().load(std::memory_order_acquire); }

bool claimLiveHub(MidiHub* hub) noexcept
{
    MidiHub* none = nullptr;
    return hub != nullptr && live().compare_exchange_strong(none, hub, std::memory_order_acq_rel);
}

void releaseLiveHub(MidiHub* hub) noexcept
{
    MidiHub* mine = hub;
    live().compare_exchange_strong(mine, nullptr, std::memory_order_acq_rel);
}

std::int64_t monotonicNs() noexcept
{
    timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000000000LL + static_cast<std::int64_t>(ts.tv_nsec);
}
} // namespace core::midi
