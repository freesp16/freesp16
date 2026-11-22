// Parameter catalogue, area SETTINGS (02-features/settings.md SET-010…017, 07-ui-design/screens/utility.md §Rows and
// sub-pages). One file per area (UID-200/201). Order band 1200 (settings, Global).
//
// The UTILITY preferences no other area registers. Every one is a preference (ParamScope::Global, saved in
// <data dir>/preferences.json by services::settings::Preferences, never in a project). Who reads them:
//   pads.threshold / pads.sensitivity      PAD THRESHOLD / SENSITIVITY 1–16, 5 = the calibration (ours: the panel's
//                                          io::PadResponse maps 5 → neutral; the consumer is the panel decoder)
//   pads.modeSelectResponse                PAD-007 / SET-016 IMMEDIATELY / OVER 1sec (the pad-mode logic)
//   lamps.brightness                       PAD/BUTTON BRIGHTNESS → io::LampBrightness (app/App.cpp io::apply)
//   settings.autosave                      AUTOSAVE OFF / 1 min / 5 min (PRJ-030) → ProjectSession autosave
//   ui.theme / ui.scale                    THEME (UID-051) → UiState::dark; UI SCALE (UID-253, host windows only)
//   ui.showCpu                             CPU USAGE OFF / ON → the status row's CPU readout (ui/shell/Shell.cpp)
//   settings.linkStartStop                 LINK SETTING START/STOP SYNC (SYN-014, P2; disabled until Link exists)
//   audio.stretchBackend                   MASTER TEMPO backend (wsola: provisional default) → StretcherBank
//   audio.bufferSize / audio.parityMode    AUDIO DEVICE page; applied at the next start (the ALSA device is fixed on
//                                          the unit)
//   audio.out.* / audio.in.afsReturn       OUTPUT CHANNEL MAP (MIX-032): the interface channel pair of each port
#include "core/params/ParamRegistry.h"

#include <cmath>
#include <cstdio>

namespace core
{
namespace
{
constexpr std::string_view responseLabels[] = {"IMMEDIATELY", "OVER 1sec"};             // SET-016 (M p.42)
constexpr std::string_view brightnessLabels[] = {"NORMAL", "DIMMED 1", "DIMMED 2", "DIMMED 3"}; // UI-159
constexpr std::string_view autosaveLabels[] = {"OFF", "1 min", "5 min"};                // PRJ-030
constexpr std::string_view themeLabels[] = {"DARK", "LIGHT"};                           // UID-051
constexpr std::string_view linkLabels[] = {"START/STOP SYNC OFF", "START/STOP SYNC ON"}; // SYN-014
constexpr std::string_view stretchLabels[] = {"WSOLA", "PVOC", "NONE"};                 // wsola first: default
constexpr std::string_view bufferLabels[] = {"64", "128", "256", "512"};                // frames per period
constexpr std::string_view offOnLabels[] = {"OFF", "ON"};
constexpr std::string_view pairLabels[] = {"OFF", "1/2", "3/4", "5/6", "7/8", "9/10", "11/12", "13/14", "15/16"};

int formatDefault5(float v, char* buf, int n) noexcept // "5 (Default)" (utility.md)
{
    const int i = static_cast<int>(std::lround(v));
    return std::snprintf(buf, static_cast<std::size_t>(n), i == 5 ? "%d (Default)" : "%d", i);
}
int formatPercent(float v, char* buf, int n) noexcept
{
    return std::snprintf(buf, static_cast<std::size_t>(n), "%ld %%", std::lround(v));
}

ParamSpec pref(std::string_view id, std::string_view label, std::string_view shortLabel, std::string_view group,
               std::span<const std::string_view> labels, float def)
{
    return {.id = id, .label = label, .shortLabel = shortLabel, .group = group, .type = ParamType::Enum,
            .minValue = 0, .maxValue = static_cast<float>(labels.size() - 1), .defaultValue = def,
            .enumLabels = labels, .scope = ParamScope::Global};
}

void addSettingsParams(ParamRegistry& r)
{
    // ---- Controllers (SET-015, PAD-040, PAD-007) ----
    r.add({.id = "pads.threshold", .label = "PAD THRESHOLD", .shortLabel = "THRESH", .group = "PADS",
           .type = ParamType::Integer, .minValue = 1, .maxValue = 16, .defaultValue = 5, .format = formatDefault5,
           .scope = ParamScope::Global});
    r.add({.id = "pads.sensitivity", .label = "PAD SENSITIVITY", .shortLabel = "SENSE", .group = "PADS",
           .type = ParamType::Integer, .minValue = 1, .maxValue = 16, .defaultValue = 5, .format = formatDefault5,
           .scope = ParamScope::Global});
    r.add(pref("pads.modeSelectResponse", "PAD MODE SELECT RESPONSE", "PADMODE", "PADS", responseLabels, 0));
    r.add(pref("lamps.brightness", "PAD/BUTTON BRIGHTNESS", "BRIGHT", "DISPLAY", brightnessLabels, 0));
    // ---- Behaviour / Display ----
    r.add(pref("settings.autosave", "AUTOSAVE", "AUTOSAVE", "PROJECT", autosaveLabels, 1));
    r.add(pref("ui.theme", "THEME", "THEME", "DISPLAY", themeLabels, 0));
    // AMP ENVELOPE's time axis re-fits itself (animated) after every change only when this is ON;
    // OFF (default) = fitted on entry / track change, widened at once only if the envelope outgrows it.
    r.add(pref("ui.envAutoZoom", "AMP ENV AUTO ZOOM", "AUTOZOOM", "DISPLAY", offOnLabels, 0));
    // The whole machine's CPU usage in the status row of every screen (ui::StatusRow), refreshed once a second.
    r.add(pref("ui.showCpu", "CPU USAGE", "CPU", "DISPLAY", offOnLabels, 0));
    r.add({.id = "ui.scale", .label = "UI SCALE", .shortLabel = "SCALE", .group = "DISPLAY", .type = ParamType::Integer,
           .minValue = 100, .maxValue = 200, .defaultValue = 100, .coarseStep = 25, .fineStep = 5,
           .format = formatPercent, .scope = ParamScope::Global});
    // ---- MIDI / sync ----
    r.add(pref("settings.linkStartStop", "LINK SETTING", "LINK", "SYNC", linkLabels, 0));
    // ---- Audio (AUDIO DEVICE page) ----
    r.add(pref("audio.stretchBackend", "STRETCH ENGINE", "STRETCH", "AUDIO", stretchLabels, 0));
    r.add(pref("audio.bufferSize", "BUFFER SIZE", "BUFFER", "AUDIO", bufferLabels, 2));
    r.add(pref("audio.parityMode", "PARITY MODE", "PARITY", "AUDIO", offOnLabels, 0));
    // ---- OUTPUT CHANNEL MAP (MIX-032): MAIN, ASSIGN 3/4 … 7/8, HEADPHONES, AFS SEND → output pair; AFS RETURN ← input
    r.add(pref("audio.out.main", "MAIN", "MAIN", "OUTPUT MAP", pairLabels, 1));
    r.add(pref("audio.out.assign34", "ASSIGN 3/4", "ASSIGN34", "OUTPUT MAP", pairLabels, 2));
    r.add(pref("audio.out.assign56", "ASSIGN 5/6", "ASSIGN56", "OUTPUT MAP", pairLabels, 3));
    r.add(pref("audio.out.assign78", "ASSIGN 7/8", "ASSIGN78", "OUTPUT MAP", pairLabels, 4));
    r.add(pref("audio.out.phones", "HEADPHONES", "PHONES", "OUTPUT MAP", pairLabels, 5));
    r.add(pref("audio.out.afsSend", "AFS SEND", "AFS SEND", "OUTPUT MAP", pairLabels, 6));
    r.add(pref("audio.in.afsReturn", "AFS RETURN", "AFS RET", "OUTPUT MAP", pairLabels, 2));
}
} // namespace
} // namespace core

APP_REGISTER_PARAMS("settings", 1200, &core::addSettingsParams)
