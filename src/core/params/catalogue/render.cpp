// Parameter catalogue, area RENDER (02-features/render-and-processing.md REN-001…005, REN-010/011; 03-ui/
// enumerations.md "Sampling and render"; 03-ui/navigation-and-workflows.md W12/W13). One file per area (UID-200/201).
// Order band 1350.
//
//   render.source      the job's source (REN-001): TRACK, PATTERN, SCENE (each pattern with content in turn),
//                      ARRANGEMENT (the song). The RENDER AUDIO screen's RENDER MODE (PATTERN / ARRANGEMENT) and its
//                      INDIVIDUAL TRACKS toggles map onto it
//   render.repeats     N repeats of the pattern / scene (REN-001)
//   render.bars        length in bars, 0 = the source's own length × repeats (REN-002)
//   render.remainder   REMAINDER (REN-002 tails, our meanings): CUT = stop at the length; LEAVE = append the FX tail
//                      until −90 dBFS or 10 s; WRAP = the same tail mixed onto the start (a seamless loop)
//   render.includeFx   TRACK source: include the send / master FX (REN-001 option)
//   render.bitDepth    16 / 24 / 32F (REN-002; the rate is the engine rate)
//   render.normalize   normalize after render (REN-002 → REN-010)
//   render.destination LIBRARY (RenderedAudio/), TO TRACK (REN-004: an empty track of the scene, undoable)
//   render.normalizeTarget  the normalize target −6.0…0.0 dBFS, default −1.0 (REN-011)
//   render.muteLive    internal engine message (never saved, not shown): 1 = fade every live output out over 64
//                      samples while a job runs, 0 = back (REN-005; engine::mixer::OutputRouter::setMuteAll)
//   render.offline     internal engine message: 1 = this engine renders offline — the TEMPORARY test tone (track 1
//                      while it has no sample, engine/stages/TestToneStage.h) stays off, so it never reaches a file
//
// All are preferences of the session (ParamScope::Global) except render.muteLive (internal).
#include "core/params/ParamRegistry.h"

#include <cstdio>

namespace core
{
namespace
{
constexpr std::string_view sourceLabels[] = {"TRACK", "PATTERN", "SCENE", "ARRANGEMENT"};
constexpr std::string_view remainderLabels[] = {"CUT", "WRAP", "LEAVE"};
constexpr std::string_view bitDepthLabels[] = {"16", "24", "32F"};
constexpr std::string_view destinationLabels[] = {"LIBRARY", "TO TRACK"};
constexpr std::string_view offOnLabels[] = {"OFF", "ON"};

int formatBars(float v, char* buf, int n) noexcept // 0 = the source's own length
{
    return v < 0.5f ? std::snprintf(buf, static_cast<std::size_t>(n), "AUTO")
                    : std::snprintf(buf, static_cast<std::size_t>(n), "%d", static_cast<int>(v + 0.5f));
}

int formatDbfs(float v, char* buf, int n) noexcept
{
    return std::snprintf(buf, static_cast<std::size_t>(n), "%.1f dBFS", static_cast<double>(v));
}

void addRenderParams(ParamRegistry& r)
{
    r.add({.id = "render.source", .label = "RENDER SOURCE", .shortLabel = "SOURCE", .group = "RENDER",
           .type = ParamType::Enum, .minValue = 0, .maxValue = 3, .defaultValue = 1, .enumLabels = sourceLabels,
           .scope = ParamScope::Global});
    r.add({.id = "render.repeats", .label = "REPEATS", .shortLabel = "REPEATS", .group = "RENDER",
           .type = ParamType::Integer, .minValue = 1, .maxValue = 64, .defaultValue = 1, .scope = ParamScope::Global});
    r.add({.id = "render.bars", .label = "LENGTH", .shortLabel = "LENGTH", .group = "RENDER",
           .type = ParamType::Integer, .minValue = 0, .maxValue = 256, .defaultValue = 0, .format = formatBars,
           .scope = ParamScope::Global});
    r.add({.id = "render.remainder", .label = "REMAINDER", .shortLabel = "REMAIN", .group = "RENDER",
           .type = ParamType::Enum, .minValue = 0, .maxValue = 2, .defaultValue = 0, .enumLabels = remainderLabels,
           .scope = ParamScope::Global});
    r.add({.id = "render.includeFx", .label = "SEND/MASTER FX", .shortLabel = "BUS FX", .group = "RENDER",
           .type = ParamType::Bool, .minValue = 0, .maxValue = 1, .defaultValue = 1, .enumLabels = offOnLabels,
           .scope = ParamScope::Global});
    r.add({.id = "render.bitDepth", .label = "BIT DEPTH", .shortLabel = "BITS", .group = "RENDER",
           .type = ParamType::Enum, .minValue = 0, .maxValue = 2, .defaultValue = 1, .enumLabels = bitDepthLabels,
           .scope = ParamScope::Global});
    r.add({.id = "render.normalize", .label = "NORMALIZE", .shortLabel = "NORMLZ", .group = "RENDER",
           .type = ParamType::Bool, .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = offOnLabels,
           .scope = ParamScope::Global});
    r.add({.id = "render.destination", .label = "DESTINATION", .shortLabel = "DEST", .group = "RENDER",
           .type = ParamType::Enum, .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = destinationLabels,
           .scope = ParamScope::Global});
    r.add({.id = "render.normalizeTarget", .label = "NORMALIZE TARGET", .shortLabel = "TARGET", .group = "RENDER",
           .type = ParamType::Continuous, .minValue = -6.0f, .maxValue = 0.0f, .defaultValue = -1.0f,
           .coarseStep = 0.5f, .fineStep = 0.1f, .format = formatDbfs, .scope = ParamScope::Global});
    // Engine message (not saved, not shown): REN-005 live mute while a job runs.
    r.add({.id = "render.muteLive", .label = "RENDER MUTE", .shortLabel = "R.MUTE", .group = "RENDER",
           .type = ParamType::Bool, .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = offOnLabels,
           .scope = ParamScope::Global, .storage = {.key = "internal.render.muteLive"}});
    r.add({.id = "render.offline", .label = "OFFLINE ENGINE", .shortLabel = "OFFLINE", .group = "RENDER",
           .type = ParamType::Bool, .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = offOnLabels,
           .scope = ParamScope::Global, .storage = {.key = "internal.render.offline"}});
}
} // namespace
} // namespace core

APP_REGISTER_PARAMS("render", 1350, &core::addRenderParams)
