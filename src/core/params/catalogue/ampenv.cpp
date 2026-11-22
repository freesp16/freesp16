// Parameter catalogue, area AMP ENVELOPE (ENV): the AMP ENVELOPE page's knobs and SETTING rows (spec
// 02-features/amp-envelope.md, 03-ui/screens.md "AMP ENVELOPE", 07-ui-design/screens/amp-envelope-lfo.md). One file
// per area (UID-200/201). Values are display indices (ENV-001): 0–127, and for HOLD and RELEASE one more detent,
// 128 = INFINITY (UI-231). The engine maps them through core/util/ampenv/EnvTime.h; files store the normalised value
// (index / 128, INFINITY 1.0) under the reference field names.
//
// `track.padVelocity` (velocity sensitivity, TRK-032 / ENV-006) is registered in track.cpp.
#include "core/params/ParamRegistry.h"
#include "core/util/ampenv/EnvTime.h"

namespace core
{
namespace
{
using ampenv::EnvStage;

std::uint8_t indexOf(float v) noexcept
{
    return ampenv::clampIndex(EnvStage::Hold, v); // 0–128 (attack never reaches 128: its spec clamps first)
}

int formatAttack(float v, char* buf, int n) noexcept
{
    return ampenv::formatEnvTime(EnvStage::Attack, indexOf(v), false, buf, n);
}
int formatHold(float v, char* buf, int n) noexcept
{
    return ampenv::formatEnvTime(EnvStage::Hold, indexOf(v), false, buf, n);
}
int formatRelease(float v, char* buf, int n) noexcept
{
    return ampenv::formatEnvTime(EnvStage::Release, indexOf(v), false, buf, n);
}

constexpr std::string_view curveLabels[] = {"LINEAR", "CURVE"};

void addAmpEnvParams(ParamRegistry& r)
{
    // ENV-005: the envelope's maximum level (mixer gain law index); CC 26, lockable (slot 11), touch-strip USER target.
    r.add({.id = "env.velocity",
           .label = "VELOCITY",
           .shortLabel = "VELOCITY",
           .group = "AMP ENVELOPE",
           .type = ParamType::Integer,
           .minValue = 0,
           .maxValue = 127,
           .defaultValue = 127,
           .coarseStep = 1,
           .fineStep = 1,
           .lockable = true,
           .lfoTarget = true, // engine TrackModSink destination
           .stripTarget = true,
           .midiCc = 26,
           .scope = ParamScope::Track,
           .storage = {.reference = "volumeIndex"}});
    // ENV-002 / ENV-011: ATTACK 0 (0.5 ms … 32 s), HOLD INFINITY, RELEASE 0; CC 27–29, lockable (slots 12–14).
    r.add({.id = "env.attack",
           .label = "ATTACK",
           .shortLabel = "ATTACK",
           .group = "AMP ENVELOPE",
           .type = ParamType::Integer,
           .minValue = 0,
           .maxValue = 127,
           .defaultValue = 0,
           .coarseStep = 1,
           .fineStep = 1,
           .format = formatAttack,
           .lockable = true,
           .lfoTarget = true, // engine TrackModSink destination
           .midiCc = 27,
           .scope = ParamScope::Track,
           .storage = {.reference = "attackNorm"}});
    r.add({.id = "env.hold",
           .label = "HOLD",
           .shortLabel = "HOLD",
           .group = "AMP ENVELOPE",
           .type = ParamType::Integer,
           .minValue = 0,
           .maxValue = 128,
           .defaultValue = 128,
           .coarseStep = 1,
           .fineStep = 1,
           .format = formatHold,
           .lockable = true,
           .lfoTarget = true, // engine TrackModSink destination
           .midiCc = 28,
           .scope = ParamScope::Track,
           .storage = {.reference = "holdNorm"}});
    r.add({.id = "env.release",
           .label = "RELEASE",
           .shortLabel = "RELEASE",
           .group = "AMP ENVELOPE",
           .type = ParamType::Integer,
           .minValue = 0,
           .maxValue = 128,
           .defaultValue = 0,
           .coarseStep = 1,
           .fineStep = 1,
           .format = formatRelease,
           .lockable = true,
           .lfoTarget = true, // engine TrackModSink destination
           .midiCc = 29,
           .scope = ParamScope::Track,
           .storage = {.reference = "releaseNorm"}});
    // ENV-003: SYNC (default OFF): the three times become note values that follow the tempo.
    r.add({.id = "env.sync",
           .label = "SYNC",
           .shortLabel = "SYNC",
           .group = "AMP ENVELOPE",
           .type = ParamType::Bool,
           .minValue = 0,
           .maxValue = 1,
           .defaultValue = 0,
           .scope = ParamScope::Track,
           .storage = {.reference = "bSync"}});
    // ENV-004: SETTING popup rows.
    r.add({.id = "env.attackCurve",
           .label = "ATTACK CURVE",
           .shortLabel = "A CURVE",
           .group = "AMP ENVELOPE",
           .type = ParamType::Enum,
           .minValue = 0,
           .maxValue = 1,
           .defaultValue = 0,
           .enumLabels = curveLabels,
           .scope = ParamScope::Track,
           .storage = {.reference = "attackCurveType"}});
    r.add({.id = "env.releaseCurve",
           .label = "RELEASE CURVE",
           .shortLabel = "R CURVE",
           .group = "AMP ENVELOPE",
           .type = ParamType::Enum,
           .minValue = 0,
           .maxValue = 1,
           .defaultValue = 0,
           .enumLabels = curveLabels,
           .scope = ParamScope::Track,
           .storage = {.reference = "releaseCurveType"}});
    // ENV-007: Retrigger (third SETTING row, OFF / ON).
    r.add({.id = "env.retrigger",
           .label = "RETRIGGER",
           .shortLabel = "RETRIG",
           .group = "AMP ENVELOPE",
           .type = ParamType::Bool,
           .minValue = 0,
           .maxValue = 1,
           .defaultValue = 0,
           .scope = ParamScope::Track,
           .storage = {.reference = "bRetriggerMode"}});
    // ENV-009: envelope bypass (set from TRACK MENU's BYPASS mode, UI-221).
    r.add({.id = "env.bypass",
           .label = "ENVELOPE BYPASS",
           .shortLabel = "ENV BYP",
           .group = "AMP ENVELOPE",
           .type = ParamType::Bool,
           .minValue = 0,
           .maxValue = 1,
           .defaultValue = 0,
           .scope = ParamScope::Track,
           .storage = {.reference = "bBypass"}});
}
} // namespace
} // namespace core

APP_REGISTER_PARAMS("ampenv", 300, &core::addAmpEnvParams)
