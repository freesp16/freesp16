// Shared reads of the ui::m1 screens (HOME, TRACK SETTING, MIXER, TRACK MENU, PLAYBACK, AMP ENVELOPE, BROWSE):
// parameter values of one track through the shell's ParamAccess (never the model or the engine directly), the pad
// colour, and the track's mode. Header-only, message thread, no allocation.
#pragma once

#include "core/params/ParamRegistry.h"
#include "ui/shell/ParamAccess.h"
#include "ui/shell/ShellApi.h"
#include "ui/shell/UiState.h"

#include <cmath>
#include <string_view>

namespace ui::m1
{
// engine::ParamChange scope of `spec` for track t (as the shell's strip binding: track, scene or project/global).
inline std::uint16_t scopeFor(const core::ParamSpec& spec, const UiState& st, int track) noexcept
{
    switch (spec.scope)
    {
    case core::ParamScope::Track:
        return st.trackScope(track);
    case core::ParamScope::Scene:
    case core::ParamScope::SceneOverrideElseProject:
        return static_cast<std::uint16_t>(st.scene - 1);
    default:
        return 0;
    }
}

// The value of `id` for track t (1–16) in the current scene, or `fallback` when the id is unknown.
inline float trackParam(ShellApi& shell, std::string_view id, int track, float fallback = 0) noexcept
{
    const auto* spec = shell.params().find(id);
    if (spec == nullptr)
        return fallback;
    return shell.values().get(*spec, scopeFor(*spec, shell.state(), track));
}
inline bool trackFlag(ShellApi& shell, std::string_view id, int track, bool fallback = false) noexcept
{
    return trackParam(shell, id, track, fallback ? 1.0f : 0.0f) >= 0.5f;
}
inline void setTrackParam(ShellApi& shell, std::string_view id, int track, float v) noexcept
{
    if (const auto* spec = shell.params().find(id))
        shell.values().set(*spec, scopeFor(*spec, shell.state(), track), spec->clamp(v));
}
// Enum label of `id` for track t ("" when unknown).
inline std::string_view trackLabel(ShellApi& shell, std::string_view id, int track) noexcept
{
    const auto* spec = shell.params().find(id);
    if (spec == nullptr || spec->enumLabels.empty())
        return {};
    const long k = std::lround(trackParam(shell, id, track) - spec->minValue);
    return k >= 0 && k < static_cast<long>(spec->enumLabels.size()) ? spec->enumLabels[static_cast<std::size_t>(k)]
                                                                    : std::string_view{};
}

// The shell's engine feeds (meters, playhead, events; polled by the shell once per frame), silent without a shell.
inline const EngineFeeds& feedsOf(const ShellApi* shell) noexcept
{
    static const EngineFeeds silent{};
    return shell != nullptr ? shell->feeds() : silent;
}

// Pad colour 1–16 of track t (track.padColor = palette index 0–15, UI-228 / TRK-031; the model's default for track t
// is its own index, so HOME and MIXER show the default palette).
inline int padColour(ShellApi& shell, int track) noexcept
{
    const auto* spec = shell.params().find("track.padColor");
    if (spec == nullptr)
        return track;
    const int v = static_cast<int>(std::lround(trackParam(shell, "track.padColor", track)));
    return (v & 15) + 1;
}

// Track mode (core::model::TrackMode order: Sample, Midi, Thru, Instrument), from track.mode, else from the HOME
// summary's type badge.
inline int trackMode(ShellApi& shell, int track) noexcept
{
    if (shell.params().find("track.mode") != nullptr)
    {
        const int m = static_cast<int>(std::lround(trackParam(shell, "track.mode", track)));
        if (m != 0)
            return m & 3;
    }
    const auto badge = shell.state().tracks[static_cast<std::size_t>((track - 1) & 15)].typeBadge;
    return badge == "MIDI" ? 1 : badge == "THRU" ? 2 : badge == "AS-1" ? 3 : 0;
}
} // namespace ui::m1

namespace ui::m1
{
// Sample-load progress per track (UI-410): 0–1 while the sample pool loads a sample for the track, < 0 otherwise.
// Written by BROWSE (the load's progress callback), read by HOME / TRACK SETTING tiles and the TRACK MENU identity row.
inline float* loadProgress() noexcept
{
    static float progress[16] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    return progress;
}
} // namespace ui::m1
