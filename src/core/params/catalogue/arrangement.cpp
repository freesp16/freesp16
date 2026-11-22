// Parameter catalogue, area ARRANGEMENT (02-features/arrangement.md ARR-002/004/005/011; 03-ui/screens.md ARRANGER
// UI-300…306). One file per area (UID-200/201). Order band 1100 (project/scene), after the scene area.
//
//   arrangement.play        ProjectSettings::playArrangement (ARR-004, rail ARRANGEMENT, default OFF); model path
//                           "arrangement.play" (core::commands::arrangement::setPlayArrangement), saved with the project
//   arrangement.loop        preference (ARR-005): what happens after the last row, STOP (default) or LOOP
//   arrangement.start       internal engine message: row ≥ 0 = start the song from that row (SHIFT + push, UI-305)
//   arrangement.row.*       the ARRANGER knobs 1–4 (ARR-011, UI-301): focus-bound values. The screen binds them to the
//                           focused row (core::commands::arrangement::rowValue loads them) and turns a knob change into
//                           editRowField (one coalesced undo step per knob, EDT-U2); not saved, never sent to the engine
//                           (the ArrangementPlan is). Knobs 5–6 are mixer.headphoneVolume / mixer.masterVolume.
//
// The engine binds play / loop / start in engine/scene/SectionSequencer.h (bindParams).
#include "core/params/ParamRegistry.h"

#include <cstdio>

namespace core
{
namespace
{
constexpr std::string_view endModes[] = {"STOP", "LOOP"}; // ARR-005

int formatOneBased(float v, char* buf, int n) noexcept // 0-based index → "1"…"16"
{
    return std::snprintf(buf, static_cast<std::size_t>(n), "%d", static_cast<int>(v + 0.5f) + 1);
}

int formatRowBpm(float v, char* buf, int n) noexcept // < 20 = "--": the row keeps the tempo in force (ARR-002)
{
    return v < 20.0f ? std::snprintf(buf, static_cast<std::size_t>(n), "--")
                     : std::snprintf(buf, static_cast<std::size_t>(n), "%.1f", static_cast<double>(v));
}

void addArrangementParams(ParamRegistry& r)
{
    r.add({.id = "arrangement.play", .label = "ARRANGEMENT", .shortLabel = "ARRANGE", .group = "ARRANGER",
           .type = ParamType::Bool, .minValue = 0, .maxValue = 1, .defaultValue = 0, .scope = ParamScope::Project,
           .storage = {.key = "arrangement.play", .reference = "bPlayArrangement"}});
    r.add({.id = "arrangement.loop", .label = "ARRANGEMENT END", .shortLabel = "ARR END", .group = "ARRANGER",
           .type = ParamType::Enum, .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = endModes,
           .scope = ParamScope::Global});
    // Engine message (not saved, not shown): start the song from row `value` (0-based); −1 = nothing.
    r.add({.id = "arrangement.start", .label = "ARRANGEMENT START", .shortLabel = "ARR STRT", .group = "ARRANGER",
           .type = ParamType::Integer, .minValue = -1, .maxValue = 998, .defaultValue = -1,
           .scope = ParamScope::Project, .storage = {.key = "internal.arrangement.start"}});

    // ---- ARRANGER knobs 1–4: the focused row (ARR-011) ----
    r.add({.id = "arrangement.row.scene", .label = "SCENE", .shortLabel = "SCENE", .group = "ARRANGER",
           .type = ParamType::Integer, .minValue = 0, .maxValue = 15, .defaultValue = 0, .format = formatOneBased,
           .scope = ParamScope::Project, .storage = {.key = "internal.arrangement.row.scene"}});
    r.add({.id = "arrangement.row.pattern", .label = "PATTERN", .shortLabel = "PATTERN", .group = "ARRANGER",
           .type = ParamType::Integer, .minValue = 0, .maxValue = 15, .defaultValue = 0, .format = formatOneBased,
           .scope = ParamScope::Project, .storage = {.key = "internal.arrangement.row.pattern"}});
    r.add({.id = "arrangement.row.repeat", .label = "REPEAT", .shortLabel = "REPEAT", .group = "ARRANGER",
           .type = ParamType::Integer, .minValue = 1, .maxValue = 99, .defaultValue = 1,
           .scope = ParamScope::Project, .storage = {.key = "internal.arrangement.row.repeat"}});
    r.add({.id = "arrangement.row.bpm", .label = "BPM", .shortLabel = "BPM", .group = "ARRANGER",
           .type = ParamType::Continuous, .minValue = 0, .maxValue = 300, .defaultValue = 0, .coarseStep = 1,
           .fineStep = 0.1f, .format = formatRowBpm, .scope = ParamScope::Project,
           .storage = {.key = "internal.arrangement.row.bpm"}});
}
} // namespace
} // namespace core

APP_REGISTER_PARAMS("arrangement", 1120, &core::addArrangementParams)
