// UTILITY sub-pages (07-ui-design/screens/utility.md "Layout — CHANNEL MAPPING and the other sub-pages", UID-880,
// UID-886): every page is the UTILITY list layout ("SettingsList": template full, PanelTitle
// with the page name, the two-column list, the same editing state, empty knob strip, breadcrumb HOME › UTILITY ›
// <PAGE>). Rows that are data (the 16 tracks of CHANNEL MAPPING, one group per MIDI port, one row per library
// location) are built by SettingsList.cpp; the fixed rows are registered here.
//   CHANNEL MAPPING (MIDI IN / OUT)  TRACK 1–16 with the track colour tick; OFF, CH 1…16 (MID-010/011; 17 values: a
//                                    ListPopup, UID-878); saved with the project, undoable
//   MIDI PORTS                       ALL PORTS (clock-loss / clock-while-stopped / output latency), then
//                                    per port IN, OUT MODE, CLOCK OFFSET (MID-002, SYN-022)
//   AUDIO DEVICE                     DEVICE, SAMPLE RATE, BUFFER SIZE, INPUT CHANNELS, PARITY MODE (settings.md Audio;
//                                    the unit's ALSA device is fixed: only BUFFER SIZE / PARITY MODE edit,
//                                    applied at the next start), + INPUT LATENCY and STRETCH ENGINE
//   OUTPUT CHANNEL MAP               MAIN, ASSIGN 3/4 … 7/8, HEADPHONES, AFS SEND, AFS RETURN (MIX-032)
//   LIBRARY LOCATIONS                LIBRARY ROOT, LOCATION 1…n, ADD LOCATION… (the spec's macOS folder chooser is the
//                                    app's own text dialog on the unit)
// CONTROLLERS and KEYBOARD MAP are separate screens (utility.controllers / utility.keyboardMap).
#include "ui/descriptors/ScreenSpec.h"
#include "ui/descriptors/SettingsSpec.h"

namespace
{
using ui::SettingsRowEntry;
using ui::SettingsRowSpec;
using ui::SettingsSectionSpec;
constexpr auto kValue = SettingsRowSpec::Kind::Value;
constexpr auto kInfo = SettingsRowSpec::Kind::Info;

constexpr ui::ScreenSpec page(std::string_view id, std::string_view title, std::string_view tab)
{
    return ui::ScreenSpec {.id = id,
                           .title = title,
                           .parent = "utility",
                           .body = ui::BodyTemplate::Full,
                           .contextBar = ui::ContextBarKind::Menu,
                           .rail = {},
                           .bodyComponent = "SettingsList",
                           .strip = {tab, {}}};
}
} // namespace

APP_REGISTER_SCREEN((page("utility.channelMapIn", "CHANNEL MAPPING (MIDI IN)", "CHANNEL MAPPING")))
APP_REGISTER_SCREEN((page("utility.channelMapOut", "CHANNEL MAPPING (MIDI OUT)", "CHANNEL MAPPING")))
APP_REGISTER_SCREEN((page("utility.midiPorts", "MIDI PORTS", "MIDI PORTS")))
APP_REGISTER_SCREEN((page("utility.audioDevice", "AUDIO DEVICE", "AUDIO DEVICE")))
APP_REGISTER_SCREEN((page("utility.outputMap", "OUTPUT CHANNEL MAP", "OUTPUT CHANNEL MAP")))
APP_REGISTER_SCREEN((page("utility.library", "LIBRARY LOCATIONS", "LIBRARY LOCATIONS")))

// ---- MIDI PORTS: the settings of every port first (MIDI preferences) ----
APP_REGISTER_SETTINGS((SettingsSectionSpec {"utility.midiPorts", "general", "ALL PORTS", 10}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility.midiPorts", "general", 10, {kValue, "CLOCK LOSS", "midi.clockLoss"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility.midiPorts", "general", 20,
                                         {kValue, "CLOCK WHILE STOPPED", "midi.clockWhileStopped"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility.midiPorts", "general", 30,
                                         {kValue, "OUTPUT LATENCY", "midi.outLatency"}}))

// ---- AUDIO DEVICE ----
APP_REGISTER_SETTINGS((SettingsSectionSpec {"utility.audioDevice", "device", {}, 10}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility.audioDevice", "device", 10, {kInfo, "DEVICE", {}, {}, "audio.device"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility.audioDevice", "device", 20,
                                         {kInfo, "SAMPLE RATE", {}, {}, "audio.format"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility.audioDevice", "device", 30, {kValue, "BUFFER SIZE", "audio.bufferSize"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility.audioDevice", "device", 40,
                                         {kInfo, "INPUT CHANNELS", {}, {}, "audio.inputs"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility.audioDevice", "device", 50, {kValue, "PARITY MODE", "audio.parityMode"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility.audioDevice", "device", 60,
                                         {kValue, "INPUT LATENCY", "live.inputLatency"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility.audioDevice", "device", 70,
                                         {kValue, "STRETCH ENGINE", "audio.stretchBackend"}}))

// ---- OUTPUT CHANNEL MAP ----
APP_REGISTER_SETTINGS((SettingsSectionSpec {"utility.outputMap", "ports", {}, 10}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility.outputMap", "ports", 10, {kValue, "MAIN", "audio.out.main"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility.outputMap", "ports", 20, {kValue, "ASSIGN 3/4", "audio.out.assign34"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility.outputMap", "ports", 30, {kValue, "ASSIGN 5/6", "audio.out.assign56"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility.outputMap", "ports", 40, {kValue, "ASSIGN 7/8", "audio.out.assign78"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility.outputMap", "ports", 50, {kValue, "HEADPHONES", "audio.out.phones"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility.outputMap", "ports", 60, {kValue, "AFS SEND", "audio.out.afsSend"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility.outputMap", "ports", 70, {kValue, "AFS RETURN", "audio.in.afsReturn"}}))
