// Parameter catalogue, SAMPLING/EDIT and LIVE SAMPLING screen values (07-ui-design/screens/sampling.md "Knob strip" and
// "EDIT list popup"; UI-320…324, UID-775…789). One file per area (UID-200/201); order band 1215 (after sampling.cpp).
// These are UI focus-bound values like arrangement.row.* — session state (scope Global, storage "session.*": never
// saved, never bound by the engine): the screens load them from the document and turn a knob change into the document
// edit:
//
//   sampling.selStart / selEnd  SELECTION START / END (frames of the track's take): the track's playback.start and
//                               playback.start + playback.length, so the trimmed take plays as selected (SAVE
//                               SELECTION writes [start, end), SMP-009)
//   sampling.editBars / editBpm the EDIT list's BAR and BPM (each follows the other, REN-021): the take's sidecar
//   live.track                  LIVE SAMPLING's TRACK (1–16; default the selected track)
//   live.saveTo                 LIVE SAMPLING's SAVE TO (LIBRARY = <library>/Recordings, SMP-025; removable locations
//                               join the list when the library reports them)
#include "core/params/ParamRegistry.h"

#include <cstdio>

namespace core
{
namespace
{
constexpr std::string_view saveToLabels[] = {"LIBRARY"};

int formatTrack(float v, char* buf, int n) noexcept
{
    return std::snprintf(buf, static_cast<std::size_t>(n), "%d", static_cast<int>(v + 0.5f) + 1);
}

void addSamplingUiParams(ParamRegistry& r)
{
    r.add({.id = "sampling.selStart", .label = "SELECTION START", .shortLabel = "SEL STRT", .group = "SAMPLING",
           .type = ParamType::Integer, .minValue = 0, .maxValue = 16777215, .defaultValue = 0, .coarseStep = 441,
           .fineStep = 1, .scope = ParamScope::Global, .storage = {.key = "session.sampling.selStart"}});
    r.add({.id = "sampling.selEnd", .label = "SELECTION END", .shortLabel = "SEL END", .group = "SAMPLING",
           .type = ParamType::Integer, .minValue = 0, .maxValue = 16777215, .defaultValue = 0, .coarseStep = 441,
           .fineStep = 1, .scope = ParamScope::Global, .storage = {.key = "session.sampling.selEnd"}});
    r.add({.id = "sampling.editBars", .label = "BAR", .shortLabel = "BAR", .group = "SAMPLING",
           .type = ParamType::Integer, .minValue = 1, .maxValue = 64, .defaultValue = 1, .coarseStep = 1,
           .fineStep = 1, .scope = ParamScope::Global, .storage = {.key = "session.sampling.editBars"}});
    r.add({.id = "sampling.editBpm", .label = "BPM", .shortLabel = "BPM", .group = "SAMPLING",
           .type = ParamType::Continuous, .minValue = 20, .maxValue = 300, .defaultValue = 120, .coarseStep = 1,
           .fineStep = 0.1f, .unit = "", .scope = ParamScope::Global, .storage = {.key = "session.sampling.editBpm"}});
    r.add({.id = "live.track", .label = "TRACK", .shortLabel = "TRACK", .group = "LIVE SAMPLING",
           .type = ParamType::Integer, .minValue = 0, .maxValue = 15, .defaultValue = 0, .coarseStep = 1,
           .fineStep = 1, .format = formatTrack, .scope = ParamScope::Global,
           .storage = {.key = "session.live.track"}});
    r.add({.id = "live.saveTo", .label = "SAVE TO", .shortLabel = "SAVE TO", .group = "LIVE SAMPLING",
           .type = ParamType::Enum, .minValue = 0, .maxValue = 0, .defaultValue = 0, .enumLabels = saveToLabels,
           .scope = ParamScope::Global, .storage = {.key = "session.live.saveTo"}});
}
} // namespace
} // namespace core

APP_REGISTER_PARAMS("samplingui", 1215, &core::addSamplingUiParams)
