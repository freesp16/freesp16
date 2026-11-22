// Parameter catalogue, area SLICE (spec 02-features/slicing.md SLC-001…006, sequencer.md "Lockable
// parameters" ParamIndex 22, 07-ui-design/screens/playback.md "SET SLICE" knob strip). One file per area (UID-200/201).
//
//   slice.number     the slice-number step lock (ParamIndex 22, 0-based 0–63, shown 1–64): SLICE pad mode records it,
//                    step entry in SLICE mode writes the last-played slice (SLC-005); the engine plays that slice
//                    (engine/voice/SliceMap.h). Lock only: a track has no stored base value (no lock = the region).
//   slice.selected … slice.area   the SET SLICE knob strip (columns 1–5). They are view values of the screen
//                    (ui/screens/setslice): SLICE No. is the screen's selection, POSITION and AREA are relative
//                    encoders shown ‹ ›, START / END read-only readouts of the current slice; the edits go through
//                    core/commands/slice (one undo step each). Not saved (scope Project, "ui." storage key,
//                    like the SCENE MANAGER's UI state).
#include "core/params/ParamRegistry.h"

#include <cmath>
#include <cstdio>

namespace core
{
namespace
{
constexpr float kMaxExactFrames = 16777215.0f; // 2^24 − 1 (playback.cpp)

int formatSlice(float v, char* buf, int n) noexcept // 0-based lock value → "1"…"64"
{
    return std::snprintf(buf, static_cast<std::size_t>(n), "%ld", std::lround(v) + 1);
}

int formatInt(float v, char* buf, int n) noexcept
{
    return std::snprintf(buf, static_cast<std::size_t>(n), "%ld", std::lround(v));
}

int formatRelative(float, char* buf, int n) noexcept
{
    return std::snprintf(buf, static_cast<std::size_t>(n), "\xE2\x80\xB9 \xE2\x80\xBA"); // ‹ ›
}

ParamSpec view(std::string_view id, std::string_view label, std::string_view shortLabel, float lo, float hi,
               float def, int (*format)(float, char*, int) noexcept, std::string_view key)
{
    return {.id = id, .label = label, .shortLabel = shortLabel, .group = "SLICE", .type = ParamType::Integer,
            .minValue = lo, .maxValue = hi, .defaultValue = def, .coarseStep = 1, .fineStep = 1, .format = format,
            .scope = ParamScope::Project, .storage = {.key = key}};
}

void addSliceParams(ParamRegistry& r)
{
    r.add({.id = "slice.number", .label = "SLICE", .shortLabel = "SLICE", .group = "SLICE", .type = ParamType::Integer,
           .minValue = 0, .maxValue = 63, .defaultValue = 0, .coarseStep = 1, .fineStep = 1, .format = formatSlice,
           .lockable = true, // ● ParamIndex 22 (SLC-005)
           .scope = ParamScope::Track});
    r.add(view("slice.selected", "SLICE No.", "SLICE No", 1, 64, 1, formatInt, "ui.slice.selected"));
    r.add(view("slice.position", "POSITION", "POSITION", -1, 1, 0, formatRelative, "ui.slice.position"));
    r.add(view("slice.start", "START", "START", 0, kMaxExactFrames, 0, formatInt, "ui.slice.start"));
    r.add(view("slice.end", "END", "END", 0, kMaxExactFrames, 0, formatInt, "ui.slice.end"));
    r.add(view("slice.area", "AREA", "AREA", -1, 1, 0, formatRelative, "ui.slice.area"));
}
} // namespace
} // namespace core

APP_REGISTER_PARAMS("slice", 250, &core::addSliceParams)
