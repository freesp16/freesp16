// UTILITY (07-ui-design/screens/utility.md "Layout — UTILITY" and §Rows; 03-ui/screens.md UI-325…328; 02-features/
// settings.md SET-001…017): the screen and its rows, in the 1.60 sections and order with Sixteen's rows appended
// (UID-876, UID-220). The list is ui/screens/utility/SettingsList (the "SettingsList" body). Rows registered elsewhere
// keep their order keys: TOUCH STRIP SETTING (USER1) / (USER2) are ui/screens/touchstrip's (order 10 / 20
// of projectCommon). Order keys step by 10 so a later package can place a row between two (never moving a reference
// row). Dropped reference rows (SET-017: LCD BRIGHTNESS, SCREEN SAVER, AUTO STANDBY, TOUCH PANEL CALIBRATION,
// CHARACTER PRIORITY, USB MASS STORAGE MODE, MIDI OUT MODE (DIN) / (USB), PRO DJ LINK SYNC SETTING) are simply not
// registered: no gap.
//
// Value rows bind (SettingsList.h): project fields (services/settings/ProjectSettings.h), the composite
// "settings.syncSource", ParamSpecs (preferences of core/params/catalogue/*.cpp, midi.* included) and
// PrefSpecs. Action rows open a page (opens = screen id) or run an action (opens = "action:<name>"); infoSource on an
// action row is the value it shows (device name, number of controllers / locations).
#include "ui/descriptors/ScreenSpec.h"
#include "ui/descriptors/SettingsSpec.h"

namespace
{
using ui::SettingsRowEntry;
using ui::SettingsRowSpec;
using ui::SettingsSectionSpec;
constexpr auto kValue = SettingsRowSpec::Kind::Value;
constexpr auto kAction = SettingsRowSpec::Kind::Action;
constexpr auto kInfo = SettingsRowSpec::Kind::Info;
} // namespace

APP_REGISTER_SCREEN((ui::ScreenSpec {.id = "utility",
                                     .title = "UTILITY",
                                     .parent = "home",
                                     .body = ui::BodyTemplate::Full,
                                     .contextBar = ui::ContextBarKind::Menu,
                                     .rail = {},
                                     .bodyComponent = "SettingsList",
                                     .strip = {"UTILITY", {}}}))
APP_REGISTER_SETTINGS((ui::SettingsSpec {"utility", "UTILITY"}))

// ---- sections (1.60 order; INFORMATION always last, UID-220) ----
APP_REGISTER_SETTINGS((SettingsSectionSpec {"utility", "projectCommon", "PROJECT SETTING (COMMON)", 10}))
APP_REGISTER_SETTINGS((SettingsSectionSpec {"utility", "midiIn", "PROJECT SETTING (MIDI IN)", 20}))
APP_REGISTER_SETTINGS((SettingsSectionSpec {"utility", "midiOut", "PROJECT SETTING (MIDI OUT)", 30}))
APP_REGISTER_SETTINGS((SettingsSectionSpec {"utility", "machine", "MACHINE SETTING", 40}))
APP_REGISTER_SETTINGS((SettingsSectionSpec {"utility", "system", "SYSTEM", 50}))
APP_REGISTER_SETTINGS((SettingsSectionSpec {"utility", "information", "INFORMATION", 1000}))

// ---- PROJECT SETTING (COMMON): 10 / 20 = TOUCH STRIP SETTING (USER1) / (USER2) ----
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "projectCommon", 30,
                                         {kValue, "TOUCH STRIP SETTING (PITCH RANGE)", "settings.pitchRange"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "projectCommon", 40,
                                         {kValue, "PATTERN QUANTIZE", "project.patternQuantize"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "projectCommon", 50,
                                         {kValue, "PAD STARTS SEQUENCE", "seq.padStartsSequence"}}))

// ---- PROJECT SETTING (MIDI IN) ----
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "midiIn", 10, {kValue, "CLOCK", "midi.in.clock"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "midiIn", 20, {kValue, "START/STOP/CONTINUE", "midi.in.startStop"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "midiIn", 30, {kValue, "NOTE/CC", "midi.in.noteCc"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "midiIn", 40,
                                         {kAction, "CHANNEL MAPPING", {}, "utility.channelMapIn"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "midiIn", 50,
                                         {kAction, "CHANNEL MAPPING RESET", {}, "action:reset.midiIn"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "midiIn", 60, {kValue, "SONG POSITION", "midi.in.songPosition"}}))

// ---- PROJECT SETTING (MIDI OUT) ----
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "midiOut", 10, {kValue, "CLOCK", "midi.out.clock"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "midiOut", 20, {kValue, "START/STOP/CONTINUE", "midi.out.startStop"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "midiOut", 30, {kValue, "NOTE/CC", "midi.out.noteCc"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "midiOut", 40,
                                         {kAction, "CHANNEL MAPPING RESET", {}, "action:reset.midiOut"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "midiOut", 50, {kValue, "SONG POSITION", "midi.out.songPosition"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "midiOut", 60, // MID-011, appended
                                         {kAction, "CHANNEL MAPPING", {}, "utility.channelMapOut"}}))

// ---- MACHINE SETTING ----
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "machine", 10,
                                         {kValue, "SYNC SOURCE SELECT", "settings.syncSource"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "machine", 20,
                                         {kAction, "MIDI PORTS", {}, "utility.midiPorts", "midi.count"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "machine", 30,
                                         {kValue, "LINK SETTING", "settings.linkStartStop", {}, "needs:sync.link"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "machine", 35, {kValue, "PAD VELOCITY", "pads.velocity"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "machine", 40, {kValue, "PAD CURVE", "pads.velocityCurve"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "machine", 50, {kValue, "PAD THRESHOLD", "pads.threshold"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "machine", 60, {kValue, "PAD SENSITIVITY", "pads.sensitivity"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "machine", 70,
                                         {kValue, "PAD MODE SELECT RESPONSE", "pads.modeSelectResponse"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "machine", 75, {kValue, "MUTE MODE", "mixer.muteMode"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "machine", 80,
                                         {kValue, "METRONOME MONITOR", "transport.metronomeMonitor"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "machine", 90,
                                         {kValue, "METRONOME VOLUME", "transport.metronomeVolume"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "machine", 100, {kValue, "COUNT IN", "transport.preCount"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "machine", 110,
                                         {kValue, "PAD/BUTTON BRIGHTNESS", "lamps.brightness"}}))
// Sixteen rows, appended (UID-220).
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "machine", 120,
                                         {kAction, "AUDIO DEVICE", {}, "utility.audioDevice", "audio.device"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "machine", 130,
                                         {kAction, "OUTPUT CHANNEL MAP", {}, "utility.outputMap"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "machine", 140,
                                         {kAction, "CONTROLLERS", {}, "utility.controllers", "controllers"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "machine", 150,
                                         {kAction, "KEYBOARD MAP", {}, "utility.keyboardMap", "needs:keyboard"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "machine", 160, {kValue, "SCENE BPM", "scene.bpmPolicy"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "machine", 170, {kValue, "RESUME", "project.resume"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "machine", 180, {kValue, "AUTOSAVE", "settings.autosave"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "machine", 190, {kValue, "THEME", "ui.theme"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "machine", 200, {kValue, "UI SCALE", "ui.scale"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "machine", 205, {kValue, "AMP ENV AUTO ZOOM", "ui.envAutoZoom"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "machine", 207, {kValue, "CPU USAGE", "ui.showCpu"}}))
// Further Sixteen preferences of settings.md (Behaviour) that no screen edits otherwise: appended.
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "machine", 210, {kValue, "PATTERN SWITCH", "scene.switchMode"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "machine", 220, {kValue, "SONG END", "arrangement.loop"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "machine", 230, {kValue, "STRIP ACTS ON", "ribbon.applyTo"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "machine", 240,
                                         {kValue, "RESTART FX LFOS ON PLAY", "fx.restartLfoOnPlay"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "machine", 250, {kValue, "SIXTEEN EXTENSIONS", "ext.enabled"}}))

// ---- SYSTEM ----
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "system", 10,
                                         {kAction, "LIBRARY LOCATIONS", {}, "utility.library", "library.count"}}))

// ---- INFORMATION (UI-328, UID-887): focusable, push does nothing ----
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "information", 10, {kInfo, "Version No.", {}, {}, "version"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "information", 20, {kInfo, "DISK SPACE", {}, {}, "diskSpace"}}))
