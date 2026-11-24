// Parameter catalogue, area MIXER (MIX): the track strip (volume, pan, send, mute, solo, send destination) and the
// project/device-wide mixer values (02-features/mixer-and-routing.md MIX-001…MIX-023, MIX-060; 07-ui-design/screens/
// mixer.md "Knob strip"). One file per area (UID-200/201). The engine binds these ids in engine::Engine
// (engine::mixer::Mixer); the gain law is core::GainLaw (fader, return, master, headphones; sends and headphones ×
// 0.5).
#include "core/params/ParamRegistry.h"
#include "core/util/GainLaw.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace core
{
namespace
{
// No dB anywhere in the levels — every level reads as its raw value: the fader-law levels 0–127
// (100 = 0 dB, unity, the default), OUTPUT TRIM 0–1023 (1023 = 0 dB). The laws themselves are unchanged.
int formatIndex(float v, char* buf, int n) noexcept
{
    return std::snprintf(buf, static_cast<std::size_t>(n), "%ld", std::lround(v));
}

int formatPan(float v, char* buf, int n) noexcept
{
    const long p = std::lround(v);
    if (p == 0)
        return std::snprintf(buf, static_cast<std::size_t>(n), "C");
    return std::snprintf(buf, static_cast<std::size_t>(n), "%c%ld", p < 0 ? 'L' : 'R', p < 0 ? -p : p);
}

constexpr std::string_view offOnLabels[] = {"OFF", "ON"};
constexpr std::string_view muteModeLabels[] = {"STOP SOUND", "STOP STEPS"};
constexpr std::string_view sendChannelLabels[] = {"SEND", "ANALOG FILTER"}; // MIX-011 (enumerations.md)

void addMixerParams(ParamRegistry& r)
{
    // ---- track strip (scope Track: scene[s].track[t]) ----
    r.add({.id = "track.volume", .label = "VOLUME", .shortLabel = "VOLUME", .group = "TRACK",
           .type = ParamType::Integer, .minValue = 0, .maxValue = 127, .defaultValue = 100, .coarseStep = 1, .fineStep = 1,
           .format = formatIndex, .lockable = true, .lfoTarget = true, .stripTarget = true, .midiCc = 7,
           .scope = ParamScope::Track, .storage = {.reference = "volume"}});
    r.add({.id = "track.pan", .label = "PAN", .shortLabel = "PAN", .group = "TRACK", .type = ParamType::Integer,
           .minValue = -64, .maxValue = 64, .defaultValue = 0, .coarseStep = 1, .fineStep = 1, .format = formatPan,
           .lockable = true, .lfoTarget = true, .stripTarget = true, .bipolar = true, .midiCc = 10,
           .scope = ParamScope::Track, .storage = {.reference = "pan"}});
    r.add({.id = "track.sendAmount", .label = "SEND AMOUNT", .shortLabel = "SEND", .group = "TRACK",
           .type = ParamType::Integer, .minValue = 0, .maxValue = 127, .defaultValue = 0, .coarseStep = 1, .fineStep = 1,
           .format = formatIndex, .lockable = true, .lfoTarget = true, .midiCc = 17, .scope = ParamScope::Track,
           .storage = {.key = "track.send.amount", .reference = "amount"}});
    r.add({.id = "track.mute", .label = "MUTE", .shortLabel = "MUTE", .group = "TRACK", .type = ParamType::Bool,
           .minValue = 0, .maxValue = 1, .defaultValue = 0, .lockable = true, .midiCc = 18, .scope = ParamScope::Track,
           .storage = {.reference = "mute"}});
    r.add({.id = "track.solo", .label = "SOLO", .shortLabel = "SOLO", .group = "TRACK", .type = ParamType::Bool,
           .minValue = 0, .maxValue = 1, .defaultValue = 0, .scope = ParamScope::Track, .storage = {.reference = "solo"}});
    // What MUTE (and another track's SOLO) does to this track — our addition, per track: STOP SOUND (default: the
    // track is silenced at once) or STOP STEPS (the sequencer skips the track's steps; what is sounding rings out,
    // pads still play). Saved in the track's extras ("native.muteMode"); the TRACK MENU's MUTE button toggles it.
    r.add({.id = "track.muteMode", .label = "MUTE MODE", .shortLabel = "MUTEMODE", .group = "TRACK",
           .type = ParamType::Enum, .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = muteModeLabels,
           .scope = ParamScope::Track});
    r.add({.id = "track.sendChannel", .label = "SEND TO", .shortLabel = "SEND TO", .group = "TRACK",
           .type = ParamType::Enum, .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = sendChannelLabels,
           .scope = ParamScope::Track, .storage = {.key = "track.send.channel", .reference = "channel"}});

    // ---- project-wide (ProjectSettings) ----
    r.add({.id = "mixer.masterVolume", .label = "MASTER", .shortLabel = "MASTER", .group = "MIXER",
           .type = ParamType::Integer, .minValue = 0, .maxValue = 127, .defaultValue = 100, .coarseStep = 1, .fineStep = 1,
           .format = formatIndex, .scope = ParamScope::Project,
           .storage = {.reference = "masterVolume"}});
    r.add({.id = "mixer.sendReturnVolume", .label = "SEND", .shortLabel = "SEND RET", .group = "MIXER",
           .type = ParamType::Integer, .minValue = 0, .maxValue = 127, .defaultValue = 100, .coarseStep = 1, .fineStep = 1,
           .format = formatIndex, .scope = ParamScope::Project,
           .storage = {.reference = "sendTrackVolume"}});
    r.add({.id = "mixer.headphoneVolume", .label = "HEADPHONES", .shortLabel = "PHONES", .group = "MIXER",
           .type = ParamType::Integer, .minValue = 0, .maxValue = 127, .defaultValue = 100, .coarseStep = 1, .fineStep = 1,
           .format = formatIndex, .scope = ParamScope::Project,
           .storage = {.reference = "headphoneVolume"}});
    r.add({.id = "mixer.softLimiter", .label = "SOFT LIMIT", .shortLabel = "SOFTLIM", .group = "MIXER",
           .type = ParamType::Enum, .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = offOnLabels,
           .scope = ParamScope::Project, .storage = {.reference = "bSoftLimitter"}});

    // ---- device preferences (SET; Sixteen's own) ----
    r.add({.id = "mixer.masterLimiter", .label = "MASTER LIMITER", .shortLabel = "MSTR LIM", .group = "MIXER",
           .type = ParamType::Enum, .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = offOnLabels,
           .scope = ParamScope::Global, .storage = {.key = "prefs.mixer.masterLimiter"}});
    r.add({.id = "mixer.outputTrim", .label = "OUTPUT TRIM", .shortLabel = "TRIM", .group = "MIXER",
           .type = ParamType::Integer, .minValue = 0, .maxValue = 1023, .defaultValue = 1023, .coarseStep = 8,
           .fineStep = 1, .format = formatIndex, .scope = ParamScope::Global,
           .storage = {.key = "prefs.mixer.outputTrim"}});
}
} // namespace
} // namespace core

APP_REGISTER_PARAMS("mixer", 500, &core::addMixerParams)
