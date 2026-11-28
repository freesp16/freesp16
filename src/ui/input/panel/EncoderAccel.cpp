// Encoder acceleration (see EncoderAccel.h).
#include "ui/input/panel/EncoderAccel.h"

#include "core/params/ParamSpec.h"

#include <algorithm>
#include <cmath>

namespace ui
{
namespace
{
// A run starts from this mean (as slow as a detent every 60 ms), so the speed level climbs over several detents of
// a steady fast turn instead of jumping on the first, uneven interval.
constexpr std::int64_t kStartMeanUs = EncoderAccel::kIdleUs / 2;
constexpr int kCountCap = 1 << 20;
} // namespace

EncoderTurn EncoderAccel::turn(int n0, int detents, double timeMs) noexcept
{
    EncoderTurn t{detents, 0, 0};
    if (n0 < 0 || n0 >= kEncoders || detents == 0)
        return t;
    auto& r = runs_[static_cast<std::size_t>(n0)];
    const auto now = static_cast<std::int64_t>(std::llround(timeMs * 1000.0));
    const int dir = detents > 0 ? 1 : -1;
    const int n = detents > 0 ? detents : -detents;
    const std::int64_t dt = std::max<std::int64_t>(0, now - r.lastUs);
    if (! r.active || dir != r.dir || dt > kIdleUs)
        r = Run{true, now, dir, 0, kStartMeanUs, 0, 0}; // a new run: reversal, pause or first turn
    else
    {
        r.lastUs = now;
        // A detent clearly slower than the level (over twice its threshold, beyond any hand's unevenness) is the turn
        // slowing down: the speed restarts from it, so the precision comes back on that detent, not a few later.
        const std::int64_t interval = dt / n;
        const int speedLevel = std::min(r.level, kSpeedLevels);
        if (speedLevel > 0 && interval > 2 * kLevelUs[static_cast<std::size_t>(speedLevel)])
            r.meanUs = interval;
        else
            r.meanUs = (3 * r.meanUs + interval) / 4;
        // The speed level the mean holds: a level already reached holds with the hysteresis margin.
        auto holds = [&](int level)
        {
            const auto limit = kLevelUs[static_cast<std::size_t>(level)];
            return r.meanUs * 100 <= limit * (std::min(r.level, kSpeedLevels) >= level ? kHysteresisPercent : 100);
        };
        int speed = 0;
        while (speed < kSpeedLevels && holds(speed + 1))
            ++speed;
        const int was = r.level;
        if (speed < std::min(r.level, kSpeedLevels) || (speed < kSpeedLevels && r.level > kSpeedLevels))
            r.level = speed; // slower: straight down to what the speed holds (the boost is lost)
        else if (r.level < speed)
            ++r.level; // faster: one level per event
        else if (r.level >= kSpeedLevels && r.level < kMaxLevel && r.atLevel >= kBoostDetents)
            ++r.level; // held at the top speed: boost
        r.atLevel = r.level != was ? 0 : std::min(kCountCap, r.atLevel + n);
    }
    const int before = r.count;
    r.count = std::min(kCountCap, before + n);
    t.warm = n - std::clamp(kWarmDetents - before, 0, n);
    t.level = r.level;
    return t;
}

void EncoderAccel::reset(int n0) noexcept
{
    if (n0 >= 0 && n0 < kEncoders)
        runs_[static_cast<std::size_t>(n0)] = {};
}

void EncoderAccel::reset() noexcept
{
    runs_ = {};
}

int EncoderAccel::maxMultiplier(long long steps) noexcept
{
    if (steps < 2LL * kSweepDetents)
        return 1;
    int m = 1;
    while (m < (1 << kMaxLevel) && 2LL * m * kSweepDetents <= steps)
        m *= 2;
    return m;
}

long long EncoderAccel::stepsOf(const core::ParamSpec& spec, bool fine) noexcept
{
    if (fine || spec.type == core::ParamType::Enum || spec.type == core::ParamType::Bool || ! (spec.coarseStep > 0))
        return 0;
    return std::llround(static_cast<double>(spec.maxValue - spec.minValue) / static_cast<double>(spec.coarseStep));
}
} // namespace ui
