#include "engine/sequencer/perf/Retrigger.h"

#include <cstring>
#include <utility>

namespace engine::sequencer::perf
{
void RetriggerScheduler::onStep(const trigger::TriggerEvent& step, RetrigSpec spec, double stepSamples) noexcept
{
    const int t = step.track & 15;
    cancel(t); // the next own step cancels the previous step's repeats
    const int count = spec.count > kMaxRetrigCount ? kMaxRetrigCount : spec.count;
    if (count < 2 || (step.kind != trigger::TriggerKind::Full && step.kind != trigger::TriggerKind::Half))
        return;
    const double divisor = retrigDivisor(spec.speedIndex);
    double len = stepSamples;
    if (! (len >= divisor))
        len = divisor; // at most one repeat per frame (only reachable far beyond 300 BPM)

    Pending& p = pending_[static_cast<std::size_t>(t)];
    p.origin = blockStart_ + (step.frame > 63 ? 63 : step.frame);
    p.step = len;
    p.divisor = divisor;
    p.next = 1;
    p.count = static_cast<std::uint8_t>(count);
    p.velocity = step.velocity;
    p.semitones = step.semitones;
    p.kind = step.kind;
    p.nextAt = p.origin + offsetOf(p, 1);

    // The step's locks, copied (its LockSet lives in a pattern object that may be retired before the last repeat).
    p.hasLocks = false;
    if (step.locks != nullptr && step.locks->values != nullptr && step.locks->mask != 0)
    {
        Locks& l = locks_[static_cast<std::size_t>(t)];
        const std::uint64_t mask = step.locks->mask & ((std::uint64_t {1} << kLockSlots) - 1);
        for (std::uint64_t m = mask; m != 0; m &= m - 1)
        {
            const int s = __builtin_ctzll(m);
            l.values[static_cast<std::size_t>(s)] = step.locks->values[s];
            l.change[static_cast<std::size_t>(s)] = step.locks->change != nullptr ? step.locks->change[s] : 0;
        }
        l.set = trigger::LockSet {mask, l.values.data(), l.change.data()};
        p.hasLocks = true;
    }
    active_ = static_cast<std::uint16_t>(active_ | (1u << t));
}

void RetriggerScheduler::swapTracks(int a, int b) noexcept
{
    if (a == b || (! pending(a) && ! pending(b)))
        return;

    std::swap(pending_[a], pending_[b]);
    const auto activeDifference = ((active_ >> a) ^ (active_ >> b)) & 1u;
    active_ ^= static_cast<std::uint16_t>((activeDifference << a) | (activeDifference << b));

    if (pending_[a].hasLocks || pending_[b].hasLocks)
    {
        // Copy the storage as bytes because the unused values have never been written.
        Locks saved;
        std::memcpy(&saved, &locks_[a], sizeof saved);
        std::memcpy(&locks_[a], &locks_[b], sizeof saved);
        std::memcpy(&locks_[b], &saved, sizeof saved);
        for (auto* locks : {&locks_[a], &locks_[b]})
        {
            locks->set.values = locks->values.data();
            locks->set.change = locks->change.data();
        }
    }
}
} // namespace engine::sequencer::perf
