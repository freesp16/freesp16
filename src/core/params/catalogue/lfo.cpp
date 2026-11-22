// Parameter catalogue, area LFO (spec 02-features/lfo.md LFO-001…011; 03-ui/enumerations.md "LFO"; data-model.md
// `Lfo`): one LFO per track, scope Track (scene[s].track[t].lfo). One file per area (UID-200/201).
// The engine binds these ids in engine::Engine (engine/modulation/LfoStage.h, kLfoParamIds) in the units below; the
// value laws are core/util/lfo/LfoLaw.h. SPEED, DEPTH and PHASE OFFSET are lockable (slots 18–20) and on CC 60–62
// (LFO-010). The destination list is generated from ParamRegistry::lfoTargets() (LFO-005).
//
// lfo.destination is the destination's ParamId value at run time (−1 = NONE): ParamIds are never written to files, so
// whoever stores it (the model's Lfo::destination / the store) keeps the ParamSpec id ("playback.pitch",
// "fx1.filter.cutoff") and converts on load; the reference importer maps ids 100–155 / 200–255.
#include "core/params/ParamRegistry.h"
#include "core/util/lfo/LfoLaw.h"

#include <cstdio>

namespace core
{
namespace
{
int formatSpeed(float v, char* buf, int n) noexcept // free speed; in SYNC the LFO screen shows the division instead
{
    const float hz = lfo::speedHz(v);
    return std::snprintf(buf, static_cast<std::size_t>(n), hz < 1.0f ? "%.2f Hz" : (hz < 10.0f ? "%.1f Hz" : "%.0f Hz"),
                         static_cast<double>(hz));
}

int formatPhase(float v, char* buf, int n) noexcept // 0–127 → 0°…357° (LfoLaw phase01)
{
    return std::snprintf(buf, static_cast<std::size_t>(n), "%d%s", static_cast<int>(lfo::phase01(v) * 360.0f + 0.5f),
                         "\xC2\xB0");
}

int formatDestination(float v, char* buf, int n) noexcept // the LFO screen shows the target's label instead
{
    if (v < 0.0f)
        return std::snprintf(buf, static_cast<std::size_t>(n), "NONE");
    return std::snprintf(buf, static_cast<std::size_t>(n), "%d", static_cast<int>(v + 0.5f));
}

// 03-ui/enumerations.md "Waveform" (the 1.60 SHAPE list, in its order) = engine::modulation::LfoShape.
constexpr std::string_view shapeLabels[] = {"SINE", "TRIANGLE", "SAW", "SQUARE", "PULSE25", "PULSE12", "RANDOM"};
// "Trigger mode": Mode FREE / TRIGGER (1.60 default) + ONE SHOT, ours (LFO-007, OQ-19); Trigger On FULL / FULL & HALF.
constexpr std::string_view modeLabels[] = {"FREE", "TRIGGER", "ONE SHOT"};
constexpr std::string_view triggerOnLabels[] = {"FULL", "FULL & HALF"};
constexpr std::string_view offOnLabels[] = {"OFF", "ON"};

void addLfoParams(ParamRegistry& r)
{
    r.add({.id = "lfo.speed", .label = "SPEED", .shortLabel = "LFO SPD", .group = "LFO", .type = ParamType::Continuous,
           .minValue = 0, .maxValue = 127, .defaultValue = 21, .coarseStep = 1, .fineStep = 0.1f,
           .format = formatSpeed, .lockable = true, .midiCc = 60, .scope = ParamScope::Track,
           .storage = {.reference = "speed"}});
    r.add({.id = "lfo.depth", .label = "DEPTH", .shortLabel = "LFO DPT", .group = "LFO", .type = ParamType::Integer,
           .minValue = 0, .maxValue = 127, .defaultValue = 63, .coarseStep = 1, .fineStep = 1, .lockable = true,
           .midiCc = 61, .scope = ParamScope::Track, .storage = {.reference = "depth"}});
    r.add({.id = "lfo.phase", .label = "PHASE OFFSET", .shortLabel = "PHASE", .group = "LFO",
           .type = ParamType::Integer, .minValue = 0, .maxValue = 127, .defaultValue = 0, .coarseStep = 1,
           .fineStep = 1, .format = formatPhase, .lockable = true, .midiCc = 62, .scope = ParamScope::Track,
           .storage = {.reference = "phaseOffset"}});
    r.add({.id = "lfo.shape", .label = "SHAPE", .shortLabel = "SHAPE", .group = "LFO", .type = ParamType::Enum,
           .minValue = 0, .maxValue = 6, .defaultValue = 0, .enumLabels = shapeLabels, .scope = ParamScope::Track,
           .storage = {.reference = "type"}});
    r.add({.id = "lfo.destination", .label = "DESTINATION", .shortLabel = "DEST", .group = "LFO",
           .type = ParamType::Integer, .minValue = -1, .maxValue = 2047, .defaultValue = -1, .coarseStep = 1,
           .fineStep = 1, .format = formatDestination, .scope = ParamScope::Track,
           .storage = {.reference = "destination"}});
    r.add({.id = "lfo.sync", .label = "SYNC", .shortLabel = "SYNC", .group = "LFO", .type = ParamType::Enum,
           .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = offOnLabels, .scope = ParamScope::Track,
           .storage = {.reference = "sync"}});
    r.add({.id = "lfo.triggerMode", .label = "MODE", .shortLabel = "MODE", .group = "LFO", .type = ParamType::Enum,
           .minValue = 0, .maxValue = 2, .defaultValue = 1, .enumLabels = modeLabels, .scope = ParamScope::Track,
           .storage = {.reference = "triggerMode"}});
    r.add({.id = "lfo.triggerOn", .label = "TRIGGER ON", .shortLabel = "TRIG ON", .group = "LFO",
           .type = ParamType::Enum, .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = triggerOnLabels,
           .scope = ParamScope::Track, .storage = {.reference = "triggerOn"}});
    r.add({.id = "lfo.randomStart", .label = "RANDOM START", .shortLabel = "RND STRT", .group = "LFO",
           .type = ParamType::Integer, .minValue = 0, .maxValue = 127, .defaultValue = 0, .coarseStep = 1,
           .fineStep = 1, .scope = ParamScope::Track, .storage = {.reference = "randStartNum"}});
    r.add({.id = "lfo.bypass", .label = "BYPASS", .shortLabel = "BYPASS", .group = "LFO", .type = ParamType::Bool,
           .minValue = 0, .maxValue = 1, .defaultValue = 0, .scope = ParamScope::Track,
           .storage = {.reference = "bypass"}});
}
} // namespace
} // namespace core

APP_REGISTER_PARAMS("lfo", 600, &core::addLfoParams)
