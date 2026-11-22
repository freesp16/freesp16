// Parameter catalogue, area TRANSPORT (02-features/sync-and-transport.md SYN-002/005/017, sequencer.md SEQ-044/045;
// 07-ui-design/screens/bpm-quantize.md). One file per area (UID-200/201). The engine binds tempo, metronome,
// metronomeVolume, nudge and nudgeAmount (engine::transport::TransportStage); transport.preCount is read by
// ui::input::transport::TransportController (Play with the bars when REC is armed); transport.metronomeMonitor is for
// the output router (until it routes the click, it plays on master + headphones).
#include "core/params/ParamRegistry.h"
#include "core/util/musictime/MusicTime.h"

#include <cmath>
#include <cstdio>

namespace core
{
namespace
{
int formatBpm(float v, char* buf, int n) noexcept // "120.0" (0.1 BPM resolution, SYN-002)
{
    return std::snprintf(buf, static_cast<std::size_t>(n), "%.1f", musictime::clampBpm(static_cast<double>(v)));
}
int formatSigned(float v, char* buf, int n) noexcept
{
    const long i = std::lround(v);
    return std::snprintf(buf, static_cast<std::size_t>(n), i > 0 ? "+%ld" : "%ld", i);
}
int formatPercent(float v, char* buf, int n) noexcept
{
    return std::snprintf(buf, static_cast<std::size_t>(n), "%ld %%", std::lround(v));
}

constexpr std::string_view offOnLabels[] = {"OFF", "ON"};
// SEQ-045: own level and output, headphones/cue by default in the spec; master + headphones until routing.
constexpr std::string_view monitorLabels[] = {"Master + Headphones", "Master", "Headphones"};
constexpr std::string_view preCountLabels[] = {"OFF", "1 BAR", "2 BARS"}; // SEQ-044

void addTransportParams(ParamRegistry& r)
{
    // SYN-002: 20.0–300.0 BPM, selector = whole BPM, SHIFT = 0.1; stored per project (ProjectSettings::tempo, the
    // reference's projectBpm).
    r.add({.id = "transport.tempo",
           .label = "BPM",
           .shortLabel = "BPM",
           .group = "TRANSPORT",
           .type = ParamType::Continuous,
           .minValue = static_cast<float>(musictime::kMinBpm),
           .maxValue = static_cast<float>(musictime::kMaxBpm),
           .defaultValue = 120.0f,
           .coarseStep = 1.0f,
           .fineStep = 0.1f,
           .format = formatBpm,
           .scope = ParamScope::Project,
           .storage = {.key = "tempo", .reference = "projectBpm"}});
    // SEQ-045 / SYN-017: the metronome toggle of the BPM/QUANTIZE panel.
    r.add({.id = "transport.metronome",
           .label = "METRONOME",
           .shortLabel = "METRO",
           .group = "TRANSPORT",
           .type = ParamType::Enum,
           .minValue = 0,
           .maxValue = 1,
           .defaultValue = 0,
           .enumLabels = offOnLabels,
           .scope = ParamScope::Project});
    r.add({.id = "transport.metronomeVolume",
           .label = "METRONOME VOLUME",
           .shortLabel = "MET VOL",
           .group = "TRANSPORT",
           .type = ParamType::Integer,
           .minValue = 0,
           .maxValue = 127,
           .defaultValue = 100,
           .coarseStep = 1,
           .fineStep = 1,
           .scope = ParamScope::Project});
    r.add({.id = "transport.metronomeMonitor",
           .label = "METRONOME MONITOR",
           .shortLabel = "MET OUT",
           .group = "TRANSPORT",
           .type = ParamType::Enum,
           .minValue = 0,
           .maxValue = 2,
           .defaultValue = 0,
           .enumLabels = monitorLabels,
           .scope = ParamScope::Project});
    // SEQ-044: the count-in before recording from stop.
    r.add({.id = "transport.preCount",
           .label = "PRE-COUNT",
           .shortLabel = "PRECOUNT",
           .group = "TRANSPORT",
           .type = ParamType::Enum,
           .minValue = 0,
           .maxValue = 2,
           .defaultValue = 0,
           .enumLabels = preCountLabels,
           .scope = ParamScope::Project});
    // SYN-005: internal, momentary (−1 / +1 while ◀ / ▶ or the NUDGE extension controls are held, 0 on release);
    // never saved — no model field.
    r.add({.id = "transport.nudge",
           .label = "NUDGE",
           .shortLabel = "NUDGE",
           .group = "TRANSPORT",
           .type = ParamType::Integer,
           .minValue = -1,
           .maxValue = 1,
           .defaultValue = 0,
           .format = formatSigned,
           .bipolar = true,
           .scope = ParamScope::Project});
    // SYN-005: the nudge is ± this percentage of the tempo, default 3 %.
    r.add({.id = "transport.nudgeAmount",
           .label = "NUDGE AMOUNT",
           .shortLabel = "NUDGEAMT",
           .group = "TRANSPORT",
           .type = ParamType::Integer,
           .minValue = 1,
           .maxValue = 10,
           .defaultValue = 3,
           .format = formatPercent,
           .scope = ParamScope::Project});
}
} // namespace
} // namespace core

APP_REGISTER_PARAMS("transport", 1000, &core::addTransportParams)
