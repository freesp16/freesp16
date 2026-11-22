#include "engine/voice/stretch/StretchMode.h"

#include <cmath>

namespace engine::voice::stretch
{
StretchMode modeFromCc(int value) noexcept
{
    // 128 values in three bands of 43 / 43 / 42 (PLY-033, our bands).
    return value <= 42 ? StretchMode::Off : (value <= 85 ? StretchMode::Resample : StretchMode::MasterTempo);
}

int ccFromMode(StretchMode mode) noexcept
{
    switch (mode)
    {
        case StretchMode::Off: return 21;
        case StretchMode::Resample: return 64;
        default: return 106;
    }
}

StretchPlan planFor(StretchMode mode, double projectBpm, double sampleBpm, float pitchCents) noexcept
{
    StretchPlan p;
    p.mode = mode;
    const double project = projectBpm > 0.0 && std::isfinite(projectBpm) ? projectBpm : kDefaultBpm;
    const bool known = sampleBpm > 0.0 && std::isfinite(sampleBpm);
    const float cents = std::isfinite(pitchCents) ? pitchCents : 0.0f;
    switch (mode)
    {
        case StretchMode::Off:
        {
            const float c = cents < -kOffMaxCents ? -kOffMaxCents : (cents > kOffMaxCents ? kOffMaxCents : cents);
            p.rate = std::exp2(static_cast<double>(c) / 1200.0);
            break;
        }
        case StretchMode::Resample:
            p.rate = known ? project / sampleBpm : 1.0;
            break;
        case StretchMode::MasterTempo:
        {
            const float m = kMasterTempoMaxCents;
            const float c = cents < -m ? -m : (cents > m ? m : cents);
            p.timeRatio = known ? sampleBpm / project : 1.0;
            p.pitchScale = std::exp2(static_cast<double>(c) / 1200.0);
            break;
        }
    }
    return p;
}

StretchPlan planFor(StretchMode mode, double projectBpm, double sampleBpm, int pitchCents) noexcept
{
    return planFor(mode, projectBpm, sampleBpm, static_cast<float>(pitchCents));
}

SampleTempo sampleTempo(double sidecarBpm, double sidecarBars, double lengthSeconds, double projectBpm) noexcept
{
    SampleTempo t;
    const bool haveLength = lengthSeconds > 0.0 && std::isfinite(lengthSeconds);
    if (sidecarBpm > 0.0 && std::isfinite(sidecarBpm))
    {
        t.bpm = sidecarBpm;
        t.bars = haveLength ? sidecarBpm * lengthSeconds / 240.0 : 0.0;
        return t;
    }
    if (! haveLength)
        return t;
    if (sidecarBars > 0.0 && std::isfinite(sidecarBars))
    {
        t.bars = sidecarBars;
        t.bpm = 240.0 * sidecarBars / lengthSeconds;
        return t;
    }
    const double project = projectBpm > 0.0 && std::isfinite(projectBpm) ? projectBpm : kDefaultBpm;
    const double raw = lengthSeconds * project / 240.0; // bars at the project tempo
    double e = std::round(std::log2(raw));
    e = e < -2.0 ? -2.0 : (e > 6.0 ? 6.0 : e);
    t.bars = std::exp2(e);
    t.bpm = 240.0 * t.bars / lengthSeconds;
    t.estimated = true;
    return t;
}
} // namespace engine::voice::stretch
