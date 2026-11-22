// Amp-envelope time map, curve and stage enums (spec 02-features/amp-envelope.md ENV-001…003, ENV-011). Pure functions
// shared by the engine (engine/modulation/AmpEnvelope*, which turns them into per-sample-rate tables once in
// prepare()) and the UI (AMP ENVELOPE screen captions, envelope thumbnail).
//
//   display index  0–127 finite; 128 = INFINITY (HOLD and RELEASE only, one detent past 127, UI-231)
//   stored value   index / 128 for 0–127, exactly 1.0 for INFINITY (ENV-001/002: finite values spread over [0, 1))
//   seconds        sync off: 0 at index 0, else tmin × (tmax / tmin)^(index / 127)     (our law)
//                    attack 0.5 ms … 32 s, hold 1 ms … 10 s (our tmin; spec "0…10 s"), release 1 ms … 20 s
//                  sync on: one of 16 musical lengths (index × 16 / 128), seconds = beats × 60 / bpm (ENV-003)
//
// The exponent uses index/127 (not the stored index/128) so the stated maxima are reached at 127.
#pragma once

#include <cstdint>

namespace core::ampenv
{
enum class EnvCurve : std::uint8_t
{
    Linear,
    Curve
};
enum class EnvStage : std::uint8_t
{
    Attack,
    Hold,
    Release
};

constexpr std::uint8_t kMaxFiniteIndex = 127; // last finite display value
constexpr std::uint8_t kInfinityIndex = 128;  // HOLD / RELEASE "INF" (attack has none: clamped to 127)
constexpr int kSyncSteps = 16;                // ENV-003: 1/64 … 4 bars

struct StageRange
{
    double minSeconds, maxSeconds; // at index 1 and 127 (index 0 = 0 s)
};
constexpr StageRange stageRange(EnvStage s) noexcept
{
    switch (s)
    {
        case EnvStage::Attack:
            return {0.0005, 32.0};
        case EnvStage::Hold:
            return {0.001, 10.0};
        case EnvStage::Release:
            break;
    }
    return {0.001, 20.0};
}
constexpr bool hasInfinity(EnvStage s) noexcept
{
    return s != EnvStage::Attack;
}
// The index the engine and the UI use for a raw value (ParamChange value, lock value): rounded, clamped to 0–127, or to
// 0–128 for a stage with INFINITY.
std::uint8_t clampIndex(EnvStage, float value) noexcept;
constexpr bool isInfinite(EnvStage s, std::uint8_t index) noexcept
{
    return hasInfinity(s) && index >= kInfinityIndex;
}

// ENV-002/003: the stage's length in seconds; +infinity for INFINITY. `bpm` is read only when `sync` (clamped 20–999).
float envSeconds(EnvStage, std::uint8_t index, bool sync, double bpm) noexcept;
double envSecondsExact(EnvStage, std::uint8_t index, bool sync, double bpm) noexcept; // the same in double (tables)

// ENV-003, ours: 1/64, 1/32, 1/16T, 1/16, 1/8T, 1/16D, 1/8, 1/4T, 1/8D, 1/4, 1/4D, 1/2, 1/2D, 1 bar, 2 bars, 4 bars.
int syncStep(std::uint8_t index) noexcept; // index × 16 / 128 → 0–15 (finite indices)
double syncBeats(int step) noexcept;       // length in quarter notes (4/4: a bar = 4 beats)
const char* syncLabel(int step) noexcept;  // "1/64" … "4 BAR"

// ENV-001: normalised storage (core::model::AmpEnvelope::attack/hold/release).
float storedFromIndex(std::uint8_t index) noexcept;
std::uint8_t indexFromStored(float) noexcept; // ≥ 1.0 → 128; callers clamp attack to 127 (clampIndex)

// Caption text of a stage value: "0 ms", "0.5 ms", "132 ms", "1.25 s", "32.0 s", "INF", or the sync label. Returns the
// number of characters written (snprintf semantics); no allocation.
int formatEnvTime(EnvStage, std::uint8_t index, bool sync, char* buf, int bufSize) noexcept;
} // namespace core::ampenv
