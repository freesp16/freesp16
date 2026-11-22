// The amp envelope's parameter block (ENV-011 defaults) and how ParamChange values reach it. The engine keeps one
// AmpEnvParams per [scene][track] (a scene switch is an index change, never a copy); it binds the ParamSpec ids below
// to its ParamTargets at set-up (EngineSetup::resolveParam) and calls applyParam() + AmpEnvelopeBank::setParams()
// from the FIFO drain.
#pragma once

#include "core/util/ampenv/EnvTime.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace engine::modulation
{
using core::ampenv::EnvCurve;

struct AmpEnvParams // [scene][track] block; ParamChange target
{
    std::uint8_t attack = 0, hold = 128, release = 0; // display index; 128 = INFINITY (hold, release), ENV-011 defaults
    std::uint8_t velocity = 127;
    bool velocitySensitive = true, retrigger = false, sync = false, bypass = false;
    EnvCurve attackCurve = EnvCurve::Linear, releaseCurve = EnvCurve::Linear;
    friend bool operator== (const AmpEnvParams&, const AmpEnvParams&) = default;
};

enum class AmpEnvField : std::uint8_t
{
    Velocity,          // "env.velocity"   0–127 (GainLaw index: the envelope's maximum level, ENV-005)
    Attack,            // "env.attack"     0–127
    Hold,              // "env.hold"       0–128 (128 = INFINITY)
    Release,           // "env.release"    0–128 (128 = INFINITY)
    Sync,              // "env.sync"       0/1
    AttackCurve,       // "env.attackCurve"  0 LINEAR, 1 CURVE
    ReleaseCurve,      // "env.releaseCurve" 0 LINEAR, 1 CURVE
    Retrigger,         // "env.retrigger"  0/1 (ENV-007)
    Bypass,            // "env.bypass"     0/1 (ENV-009)
    VelocitySensitive, // "track.padVelocity" 0/1 (registered with the track ids, TRK-032 / ENV-006)
    Count
};

struct AmpEnvParamId
{
    std::string_view id;
    AmpEnvField field;
};
// Set-up only (string ids never reach the audio thread).
inline constexpr std::array<AmpEnvParamId, static_cast<std::size_t>(AmpEnvField::Count)> kAmpEnvParamIds {{
    {"env.velocity", AmpEnvField::Velocity},
    {"env.attack", AmpEnvField::Attack},
    {"env.hold", AmpEnvField::Hold},
    {"env.release", AmpEnvField::Release},
    {"env.sync", AmpEnvField::Sync},
    {"env.attackCurve", AmpEnvField::AttackCurve},
    {"env.releaseCurve", AmpEnvField::ReleaseCurve},
    {"env.retrigger", AmpEnvField::Retrigger},
    {"env.bypass", AmpEnvField::Bypass},
    {"track.padVelocity", AmpEnvField::VelocitySensitive},
}};

// Lock slots of the envelope (core::ParamIndex, spec sequencer.md "Lockable parameters"; native units 0–127, hold and
// release 128 = INFINITY). Applied at the trigger, reverted at the next trigger (ENV-010).
constexpr int kLockSlotVelocity = 11, kLockSlotAttack = 12, kLockSlotHold = 13, kLockSlotRelease = 14;

// Writes one ParamChange value (ParamSpec native units) into the block. Real-time safe. Returns false when nothing
// changed (the caller may skip setParams()).
bool applyParam(AmpEnvParams& p, AmpEnvField field, float value) noexcept;
} // namespace engine::modulation
