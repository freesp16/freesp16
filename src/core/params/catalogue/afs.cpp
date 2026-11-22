// Parameter catalogue, area ANALOG FILTER (AFS): the analog-filter slot (02-features/analog-filter-slot.md AFS-001…
// AFS-012; 07-ui-design/screens/mixer.md "ANALOG FILTER SETTING" — knob strip afs.drive · afs.lpCutoff ·
// afs.resonance · afs.hpCutoff; 03-ui/control-surface.md §1.4, UI-116; 03-ui/enumerations.md "AFS on / route / order
// / processor / drive"). One file per area (UID-200/201); order band 520 (after the mixer). The
// engine implements every id in engine::afs::AnalogFilterSlot (engine/afs/AnalogFilterSlot.h, kAfsParamIds); the
// panel's absolute controls (afs.drive, afs.cutoff → afs.lpCutoff, afs.resonance, afs.hpCutoff, 0–1023) map 1:1.
//
// Storage: project scope, file key = the id. The reference keeps a project and a per-scene copy of the routing
// (bDaveSmithFilter, daveSmithRouteMode, daveSmithSendMode, daveSmithSendTrack; AFS-007): the scene override is not
// representable with one scope per ParamSpec, so (our choice) the project value only.
// Values of our own (no reference field): afs.processor, afs.slope, afs.envAmount, afs.envDecay,
// afs.lfoAmount (AFS-004); afs.ping (internal trigger, never saved); afs.latency (the measured external-loop latency,
// a preference: stored per device, AFS-005). AFS-009 (step locks / LFO target, P2) is not offered: lockable and
// lfoTarget stay false.
#include "core/params/ParamRegistry.h"

#include <cmath>
#include <cstdio>

namespace core
{
namespace
{
constexpr std::string_view offOnLabels[] = {"OFF", "ON"};
constexpr std::string_view insertTrackLabels[] = {"MASTER",   "TRACK 1",  "TRACK 2",  "TRACK 3",  "TRACK 4",  "TRACK 5",
                                                  "TRACK 6",  "TRACK 7",  "TRACK 8",  "TRACK 9",  "TRACK 10", "TRACK 11",
                                                  "TRACK 12", "TRACK 13", "TRACK 14", "TRACK 15", "TRACK 16"};
constexpr std::string_view insertOrderLabels[] = {"PRE FX", "POST FX"};
constexpr std::string_view processorLabels[] = {"MODEL", "EXTERNAL"};
constexpr std::string_view slopeLabels[] = {"24 dB", "12 dB"};

int formatDrive(float v, char* buf, int n) noexcept // the pot position 0–1023 (law 1 + 3·(i/1023)², F107; no dB shown)
{
    return std::snprintf(buf, static_cast<std::size_t>(n), "%ld", std::lround(v));
}

int formatHz(double hz, char* buf, int n) noexcept
{
    if (hz >= 1000.0)
        return std::snprintf(buf, static_cast<std::size_t>(n), "%.*f kHz", hz >= 10000.0 ? 1 : 2, hz / 1000.0);
    return std::snprintf(buf, static_cast<std::size_t>(n), "%.0f Hz", hz);
}

int formatLpCutoff(float v, char* buf, int n) noexcept // 20 · 1000^(i/1023): 20 Hz … 20 kHz
{
    return formatHz(20.0 * std::pow(1000.0, std::lround(v) / 1023.0), buf, n);
}

int formatHpCutoff(float v, char* buf, int n) noexcept // 0 = OFF, 1…1023 → 20 Hz … 2 kHz
{
    const long i = std::lround(v);
    if (i <= 0)
        return std::snprintf(buf, static_cast<std::size_t>(n), "OFF");
    return formatHz(20.0 * std::pow(100.0, static_cast<double>(i - 1) / 1022.0), buf, n);
}

int formatPercent1023(float v, char* buf, int n) noexcept
{
    return std::snprintf(buf, static_cast<std::size_t>(n), "%ld %%", std::lround(std::lround(v) * 100.0 / 1023.0));
}

int formatSignedPercent(float v, char* buf, int n) noexcept
{
    const long p = std::lround(v);
    return p == 0 ? std::snprintf(buf, static_cast<std::size_t>(n), "0 %%")
                  : std::snprintf(buf, static_cast<std::size_t>(n), "%+ld %%", p);
}

int formatDecay(float v, char* buf, int n) noexcept // 10 · 400^(i/127) ms (engine::afs::envDecayMs)
{
    const double ms = 10.0 * std::pow(400.0, std::lround(v) / 127.0);
    if (ms >= 1000.0)
        return std::snprintf(buf, static_cast<std::size_t>(n), "%.2f s", ms / 1000.0);
    return std::snprintf(buf, static_cast<std::size_t>(n), "%.0f ms", ms);
}

int formatLatency(float v, char* buf, int n) noexcept
{
    const long i = std::lround(v);
    if (i < 0)
        return std::snprintf(buf, static_cast<std::size_t>(n), "FAILED");
    if (i == 0)
        return std::snprintf(buf, static_cast<std::size_t>(n), "--");
    return std::snprintf(buf, static_cast<std::size_t>(n), "%ld smp", i);
}

ParamSpec afs(std::string_view id, std::string_view label, std::string_view shortLabel, ParamType type, float lo, float hi,
              float def, std::string_view key, std::string_view reference = {})
{
    return {.id = id, .label = label, .shortLabel = shortLabel, .group = "ANALOG FILTER", .type = type, .minValue = lo,
            .maxValue = hi, .defaultValue = def, .coarseStep = 1, .fineStep = 1, .scope = ParamScope::Project,
            .storage = {.key = key, .reference = reference}};
}

void addAfsParams(ParamRegistry& r)
{
    // Routing and switches (ANALOG FILTER SETTING rows 1–4).
    auto active = afs("afs.active", "ACTIVE", "ACTIVE", ParamType::Bool, 0, 1, 0, {}, "bDaveSmithFilter");
    active.enumLabels = offOnLabels;
    r.add(active);
    auto track = afs("afs.insertTrack", "INSERT TRACK", "TRACK", ParamType::Enum, 0, 16, 0, {},
                     "daveSmithSendTrack");
    track.enumLabels = insertTrackLabels;
    r.add(track);
    auto order = afs("afs.insertOrder", "INSERT ORDER", "ORDER", ParamType::Enum, 0, 1, 0, {},
                     "daveSmithSendMode");
    order.enumLabels = insertOrderLabels;
    r.add(order);
    // Default EXTERNAL: the unit's own analog filter (its LP CUTOFF / RESONANCE pots are analog only) whenever the
    // device has the send/return loop; a device without it (the desktop) runs the MODEL anyway (the engine's fallback).
    auto proc = afs("afs.processor", "PROCESSOR", "PROC", ParamType::Enum, 0, 1, 1, {});
    proc.enumLabels = processorLabels;
    r.add(proc);

    // The four panel knobs (absolute 0–1023, UI-116) and the knob strip of the popup.
    auto drive = afs("afs.drive", "DRIVE", "DRIVE", ParamType::Integer, 0, 1023, 0, {});
    drive.coarseStep = 8;
    drive.format = formatDrive;
    r.add(drive);
    auto lp = afs("afs.lpCutoff", "LP CUTOFF", "LP CUT", ParamType::Integer, 0, 1023, 1023, {});
    lp.coarseStep = 8;
    lp.format = formatLpCutoff;
    r.add(lp);
    auto res = afs("afs.resonance", "RESONANCE", "RESO", ParamType::Integer, 0, 1023, 0, {});
    res.coarseStep = 8;
    res.format = formatPercent1023;
    r.add(res);
    auto hp = afs("afs.hpCutoff", "HP CUTOFF", "HP CUT", ParamType::Integer, 0, 1023, 0, {});
    hp.coarseStep = 8;
    hp.format = formatHpCutoff;
    r.add(hp);

    // The digital model's own values (AFS-004).
    auto slope = afs("afs.slope", "SLOPE", "SLOPE", ParamType::Enum, 0, 1, 0, {});
    slope.enumLabels = slopeLabels;
    r.add(slope);
    auto env = afs("afs.envAmount", "ENV AMOUNT", "ENV AMT", ParamType::Integer, -100, 100, 0, {});
    env.format = formatSignedPercent;
    env.bipolar = true;
    r.add(env);
    auto decay = afs("afs.envDecay", "ENV DECAY", "ENV DEC", ParamType::Integer, 0, 127, 64, {});
    decay.format = formatDecay;
    r.add(decay);
    auto lfo = afs("afs.lfoAmount", "LFO AMOUNT", "LFO AMT", ParamType::Integer, -100, 100, 0, {});
    lfo.format = formatSignedPercent;
    lfo.bipolar = true;
    r.add(lfo);

    // External loop (AFS-005): the ping (internal, never saved) and the measured latency (per device: a preference).
    r.add(afs("afs.ping", "MEASURE LATENCY", "PING", ParamType::Integer, 0, 65535, 0, "internal.afs.ping"));
    auto latency = afs("afs.latency", "LOOP LATENCY", "LATENCY", ParamType::Integer, -1, 4096, 0, {});
    latency.scope = ParamScope::Global;
    latency.format = formatLatency;
    r.add(latency);
}
} // namespace
} // namespace core

APP_REGISTER_PARAMS("afs", 520, &core::addAfsParams)
