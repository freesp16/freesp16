// TIME STRETCH planning (PLY-030…034): what a voice does in each mode.
//
//   OFF           the head plays at the pitch rate 2^(cents/1200), PITCH ±24 st (PLY-020); not tempo-synced
//   RESAMPLE      the head plays at projectBpm / sampleBpm (pitch follows speed); PITCH is greyed out (PLY-032) and
//                 ignored here
//   MASTER TEMPO  the stretcher gets timeRatio = sampleBpm / projectBpm (output length / input length) and
//                 pitchScale = 2^(cents/1200) with PITCH limited to ±12 st (PLY-020); the head feeds it at unity
//
// The sample's BPM (PLY-031): the sidecar `bpm`, else `bars` (bpm = 240 · bars / length_s), else an estimate: bars =
// the nearest power of two of the sample's length at the project tempo (between 1/4 and 64 bars), shown for
// confirmation by the UI. CC 14 (PLY-033, our bands): 0–42 OFF, 43–85 RESAMPLE, 86–127 MASTER TEMPO.
// Header-only maths for the engine and the UI; no allocation, no state.
#pragma once

#include "core/model/enums/Enums.h"

namespace engine::voice::stretch
{
using core::model::StretchMode;

constexpr float kOffMaxCents = 2400.0f;          // PLY-020, OFF
constexpr float kMasterTempoMaxCents = 1200.0f;  // PLY-020, MASTER TEMPO
constexpr double kDefaultBpm = 120.0;

StretchMode modeFromCc(int value) noexcept; // PLY-033: equal thirds of 0–127
int ccFromMode(StretchMode) noexcept;       // the middle of the mode's band (21, 64, 106)

struct StretchPlan
{
    StretchMode mode = StretchMode::Off;
    double rate = 1.0;       // head increment factor (× the buffer's rate / the engine rate)
    double timeRatio = 1.0;  // stretcher: output duration / input duration
    double pitchScale = 1.0; // stretcher: frequency factor
};
// sampleBpm ≤ 0 (unknown) → RESAMPLE and MASTER TEMPO play at the sample's own tempo; projectBpm ≤ 0 → 120.
StretchPlan planFor(StretchMode mode, double projectBpm, double sampleBpm, int pitchCents) noexcept;
StretchPlan planFor(StretchMode mode, double projectBpm, double sampleBpm, float pitchCents) noexcept;

struct SampleTempo
{
    double bpm = 0.0;       // 0 = cannot be known (no length)
    double bars = 0.0;
    bool estimated = false; // PLY-031: neither bpm nor bars known — show the estimate for confirmation
};
SampleTempo sampleTempo(double sidecarBpm, double sidecarBars, double lengthSeconds, double projectBpm) noexcept;
} // namespace engine::voice::stretch
