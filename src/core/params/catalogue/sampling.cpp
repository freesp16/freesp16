// Parameter catalogue, areas SAMPLING and LIVE SAMPLING (02-features/sampling.md SMP-001…010, SMP-020…027; 03-ui/
// screens.md SAMPLING/EDIT). One file per area (UID-200/201). The engine binds these ids in
// engine/sampling/SamplingStage.h (ParamTarget::Sampling). MONITOR LEVEL is the routing area's routing.monitorLevel
// (the sampler switches routing.monitor on, SMP-003); SOFT LIMITER is the mixer's mixer.softLimiter (the input stage
// reads it, SMP-004). Order band 1210.
//
// Every value here is session state (scope Global, storage "session.*": never saved in a project, like
// routing.monitor) except live.inputLatency, a device preference: the reference keeps none of them in its project
// format, and the model has no sampling fields. Engine scopes (ParamChange::scope): sampling.rec, sampling.drop,
// live.rec, live.takeReady = (scene << 4) | track (the target track); the others 0.
#include "core/params/ParamRegistry.h"

#include <cstdio>

namespace core
{
namespace
{
// SOURCE (SMP-001): LINE IN stereo, LINE IN mono from the L or the R input, RESAMPLE (the master, REN-007).
constexpr std::string_view sourceLabels[] = {"LINE IN", "LINE IN L", "LINE IN R", "RESAMPLE"};
// REC LENGTH (SMP-005): OFF = until REC again (disk-limited), else bars at the sequencer BPM.
constexpr std::string_view lengthLabels[] = {"OFF", "1 BAR", "2 BAR", "3 BAR", "4 BAR"};
// LIVE record length (SMP-021, our list).
constexpr std::string_view liveLengthLabels[] = {"1/4 BAR", "1/2 BAR", "1 BAR", "2 BAR", "4 BAR", "8 BAR"};
constexpr std::string_view offOnLabels[] = {"OFF", "ON"};
// REC QUANTIZE (ours): the take waits for the next multiple of this many beats of the pattern (OFF = no wait).
// engine/sampling/SamplingStage.h kQuantizeBeats holds the beats of each index.
constexpr std::string_view quantizeLabels[] = {"OFF", "1 BEAT", "2 BEATS", "4 BEATS", "8 BEATS", "16 BEATS"};

// REC THRESHOLD (SMP-002): 0 = OFF, 1…41 = −60…−20 dB in 1 dB steps, 42 = PLAY.
int formatThreshold(float v, char* buf, int n) noexcept
{
    const int i = static_cast<int>(v + 0.5f);
    if (i <= 0)
        return std::snprintf(buf, static_cast<std::size_t>(n), "OFF");
    if (i >= 42)
        return std::snprintf(buf, static_cast<std::size_t>(n), "PLAY");
    return std::snprintf(buf, static_cast<std::size_t>(n), "%ddB", i - 61);
}

void addSamplingParams(ParamRegistry& r)
{
    r.add({.id = "sampling.source", .label = "SOURCE", .shortLabel = "SOURCE", .group = "SAMPLING",
           .type = ParamType::Enum, .minValue = 0, .maxValue = 3, .defaultValue = 0, .enumLabels = sourceLabels,
           .scope = ParamScope::Global, .storage = {.key = "session.sampling.source"}});
    r.add({.id = "sampling.threshold", .label = "REC THRESHOLD", .shortLabel = "THRESH", .group = "SAMPLING",
           .type = ParamType::Integer, .minValue = 0, .maxValue = 42, .defaultValue = 0, .coarseStep = 1,
           .fineStep = 1, .format = formatThreshold, .scope = ParamScope::Global,
           .storage = {.key = "session.sampling.threshold"}});
    r.add({.id = "sampling.length", .label = "REC LENGTH", .shortLabel = "LENGTH", .group = "SAMPLING",
           .type = ParamType::Enum, .minValue = 0, .maxValue = 4, .defaultValue = 0, .enumLabels = lengthLabels,
           .scope = ParamScope::Global, .storage = {.key = "session.sampling.length"}});
    // REC: 1 = standby (REC flashing, then lit once recording starts), 0 = stop / leave standby. Scope = target track.
    r.add({.id = "sampling.rec", .label = "REC", .shortLabel = "REC", .group = "SAMPLING", .type = ParamType::Bool,
           .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = offOnLabels, .scope = ParamScope::Track,
           .storage = {.key = "session.sampling.rec"}});
    // REC QUANTIZE (ours): a take starts on the next 1 / 2 / 4 / 8 / 16-beat boundary of the pattern instead of at once
    // (stopped: at PLAY). It replaces the THRESHOLD rule while on. Fixed at REC, like THRESHOLD and LENGTH.
    r.add({.id = "sampling.quantize", .label = "REC QUANTIZE", .shortLabel = "REC QNTZ", .group = "SAMPLING",
           .type = ParamType::Enum, .minValue = 0, .maxValue = 5, .defaultValue = 0, .enumLabels = quantizeLabels,
           .scope = ParamScope::Global, .storage = {.key = "session.sampling.quantize"}});
    // Drop (ours): 1 = the take of the target track (scope) is thrown away — standby or recording → Cancelled, the
    // recorder deletes its part file. A pad hit again during its own take posts it (SAMPLING/EDIT).
    r.add({.id = "sampling.drop", .label = "DROP TAKE", .shortLabel = "DROP", .group = "SAMPLING",
           .type = ParamType::Bool, .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = offOnLabels,
           .scope = ParamScope::Track, .storage = {.key = "session.sampling.drop"}});
    // PRE-ROLL (ours): a threshold start keeps this much of the input before the crossing (0 = the crossing frame).
    r.add({.id = "sampling.preRoll", .label = "PRE-ROLL", .shortLabel = "PREROLL", .group = "SAMPLING",
           .type = ParamType::Integer, .minValue = 0, .maxValue = 100, .defaultValue = 0, .coarseStep = 1,
           .fineStep = 1, .unit = "ms", .scope = ParamScope::Global, .storage = {.key = "session.sampling.preRoll"}});
    // SMP-008 / SCN-051: the SAMPLING/EDIT screen is open (the UI sets it; MIDI then ignores remote scene selection).
    r.add({.id = "sampling.screenOpen", .label = "SAMPLING OPEN", .shortLabel = "SMP OPEN", .group = "SAMPLING",
           .type = ParamType::Bool, .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = offOnLabels,
           .scope = ParamScope::Global, .storage = {.key = "session.sampling.screenOpen"}});

    r.add({.id = "live.mode", .label = "LIVE SAMPLING", .shortLabel = "LIVE", .group = "LIVE SAMPLING",
           .type = ParamType::Bool, .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = offOnLabels,
           .scope = ParamScope::Global, .storage = {.key = "session.live.mode"}});
    r.add({.id = "live.length", .label = "REC LENGTH", .shortLabel = "LENGTH", .group = "LIVE SAMPLING",
           .type = ParamType::Enum, .minValue = 0, .maxValue = 5, .defaultValue = 2, .enumLabels = liveLengthLabels,
           .scope = ParamScope::Global, .storage = {.key = "session.live.length"}});
    r.add({.id = "live.loopAfterRec", .label = "LOOP AFTER REC", .shortLabel = "LOOP REC", .group = "LIVE SAMPLING",
           .type = ParamType::Bool, .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = offOnLabels,
           .scope = ParamScope::Global, .storage = {.key = "session.live.loopAfterRec"}});
    // Arm the track (scope): 1 = start at the next pattern-quantize boundary (SMP-022), 0 = disarm / abort.
    r.add({.id = "live.rec", .label = "LIVE REC", .shortLabel = "LIVE REC", .group = "LIVE SAMPLING",
           .type = ParamType::Bool, .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = offOnLabels,
           .scope = ParamScope::Track, .storage = {.key = "session.live.rec"}});
    // NUDGE (SMP-024): frames, the engine clamps to ±1 beat at the take's tempo.
    r.add({.id = "live.nudge", .label = "NUDGE", .shortLabel = "NUDGE", .group = "LIVE SAMPLING",
           .type = ParamType::Integer, .minValue = -200000, .maxValue = 200000, .defaultValue = 0, .coarseStep = 64,
           .fineStep = 1, .unit = "smp", .bipolar = true, .scope = ParamScope::Global,
           .storage = {.key = "session.live.nudge"}});
    // Internal: the take buffer of take `value` (liveSeq) is installed on the track (scope): loop-after-rec may start.
    r.add({.id = "live.takeReady", .label = "TAKE READY", .shortLabel = "TAKE RDY", .group = "LIVE SAMPLING",
           .type = ParamType::Integer, .minValue = 0, .maxValue = 16777215, .defaultValue = 0,
           .scope = ParamScope::Track, .storage = {.key = "session.live.takeReady"}});
    // SMP-027: the device's input latency (frames), subtracted from LINE IN takes (+ the engine's own 64-frame input
    // block). A device preference; the platform layer reports none yet, so it is set per unit.
    r.add({.id = "live.inputLatency", .label = "INPUT LATENCY", .shortLabel = "IN LAT", .group = "LIVE SAMPLING",
           .type = ParamType::Integer, .minValue = 0, .maxValue = 8192, .defaultValue = 0, .coarseStep = 1,
           .fineStep = 1, .unit = "smp", .scope = ParamScope::Global, .storage = {.key = "audio.inputLatency"}});
}
} // namespace
} // namespace core

APP_REGISTER_PARAMS("sampling", 1210, &core::addSamplingParams)
