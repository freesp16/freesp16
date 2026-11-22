// Parameter catalogue, area BROWSE (LIB-010…014, UID-675…685): the BROWSE screen's knob strip and SETTING popup
// (07-ui-design/screens/browse.md "Knob strip", "SETTING popup"). All are preferences (ParamScope::Global, the settings
// file): browsing state is not part of a project. Order band 950 (our choice: between sampling/render 900 and midi
// 1000; ParamRegistry.h lists no library band).
#include "core/params/ParamRegistry.h"

#include <cstdio>

namespace core
{
namespace
{
constexpr std::string_view filterLabels[] = {"FOLDER", "TEXT"};
constexpr std::string_view sortLabels[] = {"A→Z", "Z→A"};
constexpr std::string_view offOnLabels[] = {"OFF", "ON"};
constexpr std::string_view monitorLabels[] = {"MASTER + HEADPHONES", "HEADPHONES"};
constexpr std::string_view loadToLabels[] = {"TRACK", "SLICES", "FOLDER → TRACKS"};

// browse.info: the focused file's length in seconds (BPM · BARS · location are the UI's sub-readout).
int formatLength(float seconds, char* buf, int n) noexcept
{
    if (seconds <= 0)
        return std::snprintf(buf, static_cast<std::size_t>(n), "-");
    if (seconds < 60)
        return std::snprintf(buf, static_cast<std::size_t>(n), "%.2f s", static_cast<double>(seconds));
    const int whole = static_cast<int>(seconds);
    return std::snprintf(buf, static_cast<std::size_t>(n), "%d:%02d", whole / 60, whole % 60);
}

void addLibraryParams(ParamRegistry& r)
{
    r.add({.id = "browse.filterType",
           .label = "FILTER TYPE",
           .shortLabel = "FILTER",
           .group = "BROWSE",
           .type = ParamType::Enum,
           .minValue = 0,
           .maxValue = 1,
           .defaultValue = 0,
           .enumLabels = filterLabels,
           .scope = ParamScope::Global});
    r.add({.id = "browse.sort",
           .label = "SORT",
           .shortLabel = "SORT",
           .group = "BROWSE",
           .type = ParamType::Enum,
           .minValue = 0,
           .maxValue = 1,
           .defaultValue = 0,
           .enumLabels = sortLabels,
           .scope = ParamScope::Global});
    r.add({.id = "browse.preview",
           .label = "PREVIEW",
           .shortLabel = "PREVIEW",
           .group = "BROWSE",
           .type = ParamType::Enum,
           .minValue = 0,
           .maxValue = 1,
           .defaultValue = 1,
           .enumLabels = offOnLabels,
           .scope = ParamScope::Global});
    r.add({.id = "browse.previewVolume",
           .label = "PREVIEW VOLUME",
           .shortLabel = "PRV VOL",
           .group = "BROWSE",
           .type = ParamType::Integer,
           .minValue = 0,
           .maxValue = 127,
           .defaultValue = 50,
           .coarseStep = 1,
           .fineStep = 1,
           .scope = ParamScope::Global});
    r.add({.id = "browse.previewMonitor",
           .label = "PREVIEW MONITOR",
           .shortLabel = "MONITOR",
           .group = "BROWSE",
           .type = ParamType::Enum,
           .minValue = 0,
           .maxValue = 1,
           .defaultValue = 0,
           .enumLabels = monitorLabels,
           .scope = ParamScope::Global});
    r.add({.id = "browse.loadTo",
           .label = "LOAD TO",
           .shortLabel = "LOAD TO",
           .group = "BROWSE",
           .type = ParamType::Enum,
           .minValue = 0,
           .maxValue = 2,
           .defaultValue = 0,
           .enumLabels = loadToLabels,
           .scope = ParamScope::Global});
    // Read-only display (no detents): the UI sets the value from the focused Entry, it is never stored.
    r.add({.id = "browse.info",
           .label = "INFO",
           .shortLabel = "INFO",
           .group = "BROWSE",
           .type = ParamType::Time,
           .minValue = 0,
           .maxValue = 86400,
           .defaultValue = 0,
           .coarseStep = 0,
           .fineStep = 0,
           .unit = "s",
           .format = formatLength,
           .scope = ParamScope::Global});
}
} // namespace
} // namespace core

APP_REGISTER_PARAMS("library", 950, &core::addLibraryParams)
