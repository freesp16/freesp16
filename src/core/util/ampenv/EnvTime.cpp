#include "core/util/ampenv/EnvTime.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace core::ampenv
{
namespace
{
// Quarter-note lengths of the 16 sync steps (ENV-003, our list).
constexpr double kSyncBeats[kSyncSteps] = {
    1.0 / 16, 1.0 / 8, 1.0 / 6, 1.0 / 4, 1.0 / 3, 3.0 / 8, 1.0 / 2, 2.0 / 3,
    3.0 / 4,  1.0,     3.0 / 2, 2.0,     3.0,     4.0,     8.0,     16.0,
};
constexpr const char* kSyncLabels[kSyncSteps] = {
    "1/64", "1/32", "1/16T", "1/16", "1/8T", "1/16D", "1/8",   "1/4T",
    "1/8D", "1/4",  "1/4D",  "1/2",  "1/2D", "1 BAR", "2 BAR", "4 BAR",
};
} // namespace

std::uint8_t clampIndex(EnvStage s, float value) noexcept
{
    const float top = hasInfinity(s) ? static_cast<float>(kInfinityIndex) : static_cast<float>(kMaxFiniteIndex);
    if (! (value > 0.0f)) // also NaN
        return 0;
    const float r = std::floor(value + 0.5f);
    return static_cast<std::uint8_t>(r > top ? top : r);
}

int syncStep(std::uint8_t index) noexcept
{
    const int i = index > kMaxFiniteIndex ? kMaxFiniteIndex : index;
    return i * kSyncSteps / 128;
}

double syncBeats(int step) noexcept
{
    return kSyncBeats[step < 0 ? 0 : (step >= kSyncSteps ? kSyncSteps - 1 : step)];
}

const char* syncLabel(int step) noexcept
{
    return kSyncLabels[step < 0 ? 0 : (step >= kSyncSteps ? kSyncSteps - 1 : step)];
}

double envSecondsExact(EnvStage s, std::uint8_t index, bool sync, double bpm) noexcept
{
    if (isInfinite(s, index))
        return std::numeric_limits<double>::infinity();
    if (index > kMaxFiniteIndex)
        index = kMaxFiniteIndex;
    if (sync)
    {
        const double b = bpm < 20.0 ? 20.0 : (bpm > 999.0 ? 999.0 : bpm);
        return syncBeats(syncStep(index)) * 60.0 / b;
    }
    if (index == 0)
        return 0.0;
    const auto r = stageRange(s);
    return r.minSeconds * std::pow(r.maxSeconds / r.minSeconds, static_cast<double>(index) / kMaxFiniteIndex);
}

float envSeconds(EnvStage s, std::uint8_t index, bool sync, double bpm) noexcept
{
    return static_cast<float>(envSecondsExact(s, index, sync, bpm));
}

float storedFromIndex(std::uint8_t index) noexcept
{
    return index >= kInfinityIndex ? 1.0f : static_cast<float>(index) / 128.0f;
}

std::uint8_t indexFromStored(float v) noexcept
{
    if (v >= 1.0f)
        return kInfinityIndex;
    if (! (v > 0.0f))
        return 0;
    const float r = std::floor(v * 128.0f + 0.5f);
    return static_cast<std::uint8_t>(r > kMaxFiniteIndex ? kMaxFiniteIndex : r);
}

int formatEnvTime(EnvStage s, std::uint8_t index, bool sync, char* buf, int bufSize) noexcept
{
    const auto n = static_cast<std::size_t>(bufSize > 0 ? bufSize : 0);
    if (isInfinite(s, index))
        return std::snprintf(buf, n, "INF");
    if (sync)
        return std::snprintf(buf, n, "%s", syncLabel(syncStep(index)));
    const double t = envSecondsExact(s, index, false, 120.0);
    if (t < 0.01)
        return t == 0.0 ? std::snprintf(buf, n, "0 ms") : std::snprintf(buf, n, "%.1f ms", t * 1000.0);
    if (t < 1.0)
        return std::snprintf(buf, n, "%.0f ms", t * 1000.0);
    if (t < 10.0)
        return std::snprintf(buf, n, "%.2f s", t);
    return std::snprintf(buf, n, "%.1f s", t);
}
} // namespace core::ampenv
