// Parameter catalogue, area PLAYBACK (PLY): the sample playback values of the HOME knob strip and the PLAYBACK page
// (07-ui-design/screens/home.md "Knob strip"; PLY-005: PITCH, START, LOOP START, LENGTH, LOOP + GRID SNAP, REVERSE).
// One file per area (UID-200/201). The engine's units are the ParamSpec units below (engine/voice/PlaybackParams.h).
//
// START / LOOP START / LENGTH are frames. Their real range and coarse step depend on the loaded sample (PLY-001/002:
// 0 … frames, coarse = frames / 128, SHIFT = 1 frame, core/util/playback/Grid.h) and come from the UI framework's range
// provider; the static range here is the float-exact frame range with a generic coarse step.
#include "core/params/ParamRegistry.h"

#include <cmath>
#include <cstdio>

namespace core
{
namespace
{
constexpr float kMaxExactFrames = 16777215.0f; // 2^24 − 1: frame positions travel exactly as float

int formatPitch(float v, char* buf, int n) noexcept
{
    if (std::fabs(v) < 0.05f)
        return std::snprintf(buf, static_cast<std::size_t>(n), "0.0");
    return std::snprintf(buf, static_cast<std::size_t>(n), "%+.1f", static_cast<double>(v));
}

int formatFrames(float v, char* buf, int n) noexcept
{
    return std::snprintf(buf, static_cast<std::size_t>(n), "%ld", std::lround(v));
}

constexpr std::string_view stretchLabels[] = {"OFF", "RESAMPLE", "MASTER TEMPO"};
constexpr std::string_view offOnLabels[] = {"OFF", "ON"};

ParamSpec frames(std::string_view id, std::string_view label, std::string_view shortLabel, float defaultValue,
                 std::int16_t cc, std::string_view reference)
{
    // Lockable (ParamIndex 1–3, frames).
    return {.id = id, .label = label, .shortLabel = shortLabel, .group = "PLAYBACK", .type = ParamType::Integer,
            .minValue = 0, .maxValue = kMaxExactFrames, .defaultValue = defaultValue, .coarseStep = 128,
            .fineStep = 1, .format = formatFrames, .lockable = true, .midiCc = cc, .scope = ParamScope::Track,
            .storage = {.reference = reference}};
}

void addPlaybackParams(ParamRegistry& r)
{
    r.add({.id = "playback.pitch", .label = "PITCH", .shortLabel = "PITCH", .group = "PLAYBACK",
           .type = ParamType::Continuous, .minValue = -24, .maxValue = 24, .defaultValue = 0, .coarseStep = 1,
           .fineStep = 0.1f, .unit = "st", .format = formatPitch, .lockable = true, .lfoTarget = true,
           .stripTarget = true, .bipolar = true, .midiCc = 20, .scope = ParamScope::Track,
           .storage = {.reference = "pitchCent"}});
    r.add({.id = "playback.timeStretch", .label = "TIME STRETCH", .shortLabel = "STRETCH", .group = "PLAYBACK",
           .type = ParamType::Enum, .minValue = 0, .maxValue = 2, .defaultValue = 0, .enumLabels = stretchLabels,
           .lockable = true, .midiCc = 14, .scope = ParamScope::Track, .storage = {.reference = "stretchMode"}});
    r.add({.id = "playback.loop", .label = "LOOP", .shortLabel = "LOOP", .group = "PLAYBACK", .type = ParamType::Enum,
           .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = offOnLabels, .lockable = true, .midiCc = 15,
           .scope = ParamScope::Track, .storage = {.reference = "loop"}});
    r.add(frames("playback.start", "START", "START", 0, 21, "startSample"));
    r.add(frames("playback.loopStart", "LOOP START", "LP START", 0, 22, "loopStartSample"));
    r.add(frames("playback.length", "LENGTH", "LENGTH", kMaxExactFrames, 23, "lengthSample")); // whole sample
    r.add({.id = "playback.reverse", .label = "REVERSE", .shortLabel = "REVERSE", .group = "PLAYBACK",
           .type = ParamType::Bool, .minValue = 0, .maxValue = 1, .defaultValue = 0,
           .lockable = true, // ● ParamIndex 21
           .scope = ParamScope::Track, .storage = {.reference = "bReverse"}});
    r.add({.id = "playback.gridSnap", .label = "GRID SNAP", .shortLabel = "GRIDSNAP", .group = "PLAYBACK",
           .type = ParamType::Bool, .minValue = 0, .maxValue = 1, .defaultValue = 0, .scope = ParamScope::Track});
    r.add({.id = "playback.bypass", .label = "BYPASS", .shortLabel = "BYPASS", .group = "PLAYBACK",
           .type = ParamType::Bool, .minValue = 0, .maxValue = 1, .defaultValue = 0, .scope = ParamScope::Track,
           .storage = {.reference = "bBypass"}});
}
} // namespace
} // namespace core

APP_REGISTER_PARAMS("playback", 200, &core::addPlaybackParams)
