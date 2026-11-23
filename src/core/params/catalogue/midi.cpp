// Parameter catalogue, area MIDI (spec 02-features/midi.md MID-002/010/011/012/040…047/050/070, sync-and-transport.md
// SYN-010/012/013/020/021, scenes-and-patterns.md SCN-051). One file per area (UID-200/201). The engine binds every id
// below in engine/midi/MidiEngine (ParamTarget::Midi). Order band 1010 (after transport 1000).
//
// Engine scopes (ParamChange::scope):
//   midi.in.* / midi.out.* enables                          0 (project, MidiSettingData Input / Output)
//   midi.in.channel / midi.out.channel                      the track 0–15 (project-wide per track, Input/Output
//                                                           .channelNo[16]); ParamSpec default 1 = track 1's (ours:
//                                                           the model's default is track n → channel n)
//   midi.sync* / midi.clock* / midi.outLatency               0 (preferences, SET)
//   midi.portMode / midi.portOffset                          the output port 0–7 (preferences)
//   midi.remoteSelectBlocked                                 0 (internal: the UI sets it while SAMPLING/EDIT is open)
//   MIDI track values (midi.note1 … midi.mode)               (scene << 4) | track (Scene.track[t].midi)
// MIDI track values are not lockable here (open item: SEQ-050 locks need lock slots beyond core::ParamIndex's
// 23 + 12 FX slots, i.e. a change to the LockSet layout).
#include "core/params/ParamRegistry.h"

#include <cstdio>

namespace core
{
namespace
{
constexpr std::string_view offOnLabels[] = {"OFF", "ON"};
constexpr std::string_view syncSourceLabels[] = {"INTERNAL", "MIDI CLOCK"}; // SYN-010 (Ableton Link: P2)
constexpr std::string_view clockLossLabels[] = {"KEEP TEMPO", "STOP"};      // SYN-013
constexpr std::string_view portModeLabels[] = {"OUT", "OUT+MERGE A", "OUT+MERGE B", "THRU A", "THRU B"}; // MID-002
constexpr std::string_view midiModeLabels[] = {"NOTES", "DRUM MAP"};        // MID-045

int formatChannel(float v, char* buf, int n) noexcept // 0 = OFF, 1–16
{
    const int c = static_cast<int>(v + 0.5f);
    return c <= 0 ? std::snprintf(buf, static_cast<std::size_t>(n), "OFF")
                  : std::snprintf(buf, static_cast<std::size_t>(n), "CH.%d", c);
}
int formatOffOr(float v, char* buf, int n) noexcept // −1 = OFF ("--" on the reference's popups), else the value
{
    const int i = static_cast<int>(v < 0.0f ? v - 0.5f : v + 0.5f);
    return i < 0 ? std::snprintf(buf, static_cast<std::size_t>(n), "OFF")
                 : std::snprintf(buf, static_cast<std::size_t>(n), "%d", i);
}
int formatNoteOrOff(float v, char* buf, int n) noexcept
{
    static constexpr const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const int i = static_cast<int>(v < 0.0f ? v - 0.5f : v + 0.5f);
    if (i < 0)
        return std::snprintf(buf, static_cast<std::size_t>(n), "OFF");
    return std::snprintf(buf, static_cast<std::size_t>(n), "%s%d", names[i % 12], i / 12 - 1); // C4 = 60
}
int formatHarmony(float v, char* buf, int n) noexcept // NOTE2–4: semitones from NOTE, 0 = OFF (−127 ~ OFF ~ +127)
{
    const int i = static_cast<int>(v < 0.0f ? v - 0.5f : v + 0.5f);
    return i == 0 ? std::snprintf(buf, static_cast<std::size_t>(n), "OFF")
                  : std::snprintf(buf, static_cast<std::size_t>(n), "%+d", i);
}
int formatVelocity(float v, char* buf, int n) noexcept // 0 = OFF (step/pad velocity)
{
    const int i = static_cast<int>(v + 0.5f);
    return i <= 0 ? std::snprintf(buf, static_cast<std::size_t>(n), "OFF")
                  : std::snprintf(buf, static_cast<std::size_t>(n), "%d", i);
}
int formatSteps(float v, char* buf, int n) noexcept
{
    return std::snprintf(buf, static_cast<std::size_t>(n), "%.2f", static_cast<double>(v));
}
int formatMs(float v, char* buf, int n) noexcept
{
    return std::snprintf(buf, static_cast<std::size_t>(n), "%+d ms", static_cast<int>(v < 0 ? v - 0.5f : v + 0.5f));
}

// Static id/label tables (ParamSpec holds string_views).
constexpr std::string_view kEnableIds[2][4] = {
    {"midi.in.clock", "midi.in.startStop", "midi.in.noteCc", "midi.in.songPosition"},
    {"midi.out.clock", "midi.out.startStop", "midi.out.noteCc", "midi.out.songPosition"}};
constexpr std::string_view kEnableLabels[4] = {"CLOCK", "START/STOP", "NOTE/CC", "SONG POSITION"};
constexpr std::string_view kEnableShort[4] = {"CLOCK", "ST/STOP", "NOTE/CC", "SPP"};
constexpr std::string_view kEnableRefs[4] = {"bUseClock", "bUseStartStop", "bUseNoteCC", "bUseSongPosition"};
constexpr float kEnableDefaults[4] = {1, 1, 1, 1}; // core::model::MidiPortSettings (utility.md: ENABLE)

constexpr std::string_view kNoteIds[4] = {"midi.note1", "midi.note2", "midi.note3", "midi.note4"};
constexpr std::string_view kNoteLabels[4] = {"NOTE 1", "NOTE 2", "NOTE 3", "NOTE 4"};
constexpr std::string_view kNoteRefs[4] = {"note1", "note2", "note3", "note4"};

constexpr std::string_view kCc1Num[4] = {"midi.cc1.cc3", "midi.cc1.cc4", "midi.cc1.cc5", "midi.cc1.cc6"};
constexpr std::string_view kCc1Val[4] = {"midi.cc1.value3", "midi.cc1.value4", "midi.cc1.value5", "midi.cc1.value6"};
constexpr std::string_view kCc1Act[4] = {"midi.cc1.active3", "midi.cc1.active4", "midi.cc1.active5",
                                         "midi.cc1.active6"};
constexpr std::string_view kCc2Num[6] = {"midi.cc2.cc1", "midi.cc2.cc2", "midi.cc2.cc3",
                                         "midi.cc2.cc4", "midi.cc2.cc5", "midi.cc2.cc6"};
constexpr std::string_view kCc2Val[6] = {"midi.cc2.value1", "midi.cc2.value2", "midi.cc2.value3",
                                         "midi.cc2.value4", "midi.cc2.value5", "midi.cc2.value6"};
constexpr std::string_view kCc2Act[6] = {"midi.cc2.active1", "midi.cc2.active2", "midi.cc2.active3",
                                         "midi.cc2.active4", "midi.cc2.active5", "midi.cc2.active6"};
constexpr std::string_view kParamLabel[6] = {"PARAM 1", "PARAM 2", "PARAM 3", "PARAM 4", "PARAM 5", "PARAM 6"};
constexpr std::string_view kParamShort[6] = {"PARAM1", "PARAM2", "PARAM3", "PARAM4", "PARAM5", "PARAM6"};
constexpr std::string_view kCcLabel[6] = {"CC NO.1", "CC NO.2", "CC NO.3", "CC NO.4", "CC NO.5", "CC NO.6"};
constexpr std::string_view kActLabel[6] = {"ACTIVE 1", "ACTIVE 2", "ACTIVE 3", "ACTIVE 4", "ACTIVE 5", "ACTIVE 6"};
constexpr float kCc1Defaults[4] = {7, 1, 2, 10};       // MID-041 (MidiCc1Data)
constexpr float kCc2Defaults[6] = {71, 72, 73, 74, 75, 76}; // MID-042 (MidiCc2Data)

void addMidiParams(ParamRegistry& r)
{
    // ---- MID-012: global enables per direction (project, MidiSettingData Input / Output) ----
    for (int dir = 0; dir < 2; ++dir)
        for (int i = 0; i < 4; ++i)
            r.add({.id = kEnableIds[dir][i], .label = kEnableLabels[i], .shortLabel = kEnableShort[i],
                   .group = dir == 0 ? "MIDI IN" : "MIDI OUT", .type = ParamType::Bool, .minValue = 0, .maxValue = 1,
                   .defaultValue = kEnableDefaults[i], .enumLabels = offOnLabels, .scope = ParamScope::Project,
                   .storage = {.reference = kEnableRefs[i]}});
    // ---- MID-010 / MID-011: receive / send channel per track (project; scope = track) ----
    r.add({.id = "midi.in.channel", .label = "RECEIVE CH", .shortLabel = "RX CH", .group = "MIDI IN",
           .type = ParamType::Integer, .minValue = 0, .maxValue = 16, .defaultValue = 1, .format = formatChannel,
           .scope = ParamScope::Project, .storage = {.reference = "Input.channelNo"}});
    r.add({.id = "midi.out.channel", .label = "SEND CH", .shortLabel = "TX CH", .group = "MIDI OUT",
           .type = ParamType::Integer, .minValue = 0, .maxValue = 16, .defaultValue = 1, .format = formatChannel,
           .scope = ParamScope::Project, .storage = {.reference = "Output.channelNo"}});

    // ---- SYN-010…021, MID-002, MID-070: preferences (SET) ----
    r.add({.id = "midi.syncSource", .label = "SYNC SOURCE", .shortLabel = "SYNC", .group = "SYNC",
           .type = ParamType::Enum, .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = syncSourceLabels,
           .scope = ParamScope::Global});
    r.add({.id = "midi.syncPort", .label = "SYNC PORT", .shortLabel = "PORT", .group = "SYNC",
           .type = ParamType::Integer, .minValue = 1, .maxValue = 8, .defaultValue = 1, .scope = ParamScope::Global});
    r.add({.id = "midi.clockLoss", .label = "CLOCK LOSS", .shortLabel = "LOSS", .group = "SYNC",
           .type = ParamType::Enum, .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = clockLossLabels,
           .scope = ParamScope::Global});
    r.add({.id = "midi.clockWhileStopped", .label = "CLOCK WHILE STOPPED", .shortLabel = "CLK STOP", .group = "SYNC",
           .type = ParamType::Bool, .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = offOnLabels,
           .scope = ParamScope::Global});
    // The audio output latency the sender adds to every deadline (frames leave the engine at the period start;
    // the DAC plays them a buffer later): default 12 ms ≈ 2 × 256-frame periods at 44.1 kHz; confirmed on the unit.
    r.add({.id = "midi.outLatency", .label = "OUTPUT LATENCY", .shortLabel = "LATENCY", .group = "MIDI OUT",
           .type = ParamType::Integer, .minValue = 0, .maxValue = 100, .defaultValue = 12, .unit = "ms",
           .scope = ParamScope::Global});
    r.add({.id = "midi.portMode", .label = "PORT MODE", .shortLabel = "MODE", .group = "MIDI OUT",
           .type = ParamType::Enum, .minValue = 0, .maxValue = 4, .defaultValue = 0, .enumLabels = portModeLabels,
           .scope = ParamScope::Global});
    r.add({.id = "midi.portOffset", .label = "PORT OFFSET", .shortLabel = "OFFSET", .group = "MIDI OUT",
           .type = ParamType::Integer, .minValue = -50, .maxValue = 50, .defaultValue = 0, .format = formatMs,
           .scope = ParamScope::Global});
    // SCN-051: CC 0 / 32 remote selection is ignored while SAMPLING/EDIT is open (internal, never saved).
    r.add({.id = "midi.remoteSelectBlocked", .label = "REMOTE SELECT BLOCKED", .shortLabel = "BLOCKED",
           .group = "MIDI IN", .type = ParamType::Bool, .minValue = 0, .maxValue = 1, .defaultValue = 0,
           .enumLabels = offOnLabels, .scope = ParamScope::Project,
           .storage = {.key = "internal.midi.remoteSelectBlocked"}});

    // ---- MIDI tracks (MID-040…047, MID-050; Scene.track[t].midi) ----
    // NOTE: the base note C-1…G9; NOTE2–4: the chord's other notes as semitones from NOTE, −127 ~ OFF (0) ~ +127 (the
    // reference's MidiNoteData note2–4; the manual's "MIDI NOTE settings"), so a new NOTE moves the whole chord.
    for (int i = 0; i < 4; ++i)
        r.add({.id = kNoteIds[i], .label = kNoteLabels[i], .shortLabel = kNoteLabels[i], .group = "NOTE",
               .type = i == 0 ? ParamType::Note : ParamType::Integer, .minValue = i == 0 ? 0.0f : -127.0f,
               .maxValue = 127, .defaultValue = i == 0 ? 60.0f : 0.0f,
               .format = i == 0 ? formatNoteOrOff : formatHarmony, .bipolar = i != 0, .scope = ParamScope::Track,
               .storage = {.reference = kNoteRefs[i]}});
    r.add({.id = "midi.velocity", .label = "VELOCITY", .shortLabel = "VELOCITY", .group = "NOTE",
           .type = ParamType::Integer, .minValue = 0, .maxValue = 127, .defaultValue = 100, .format = formatVelocity,
           .scope = ParamScope::Track, .storage = {.reference = "velocity"}});
    r.add({.id = "midi.length", .label = "LENGTH", .shortLabel = "LENGTH", .group = "NOTE",
           .type = ParamType::Continuous, .minValue = 0.25f, .maxValue = 64.0f, .defaultValue = 1.0f,
           .coarseStep = 0.25f, .fineStep = 0.25f, .format = formatSteps, .scope = ParamScope::Track,
           .storage = {.reference = "length"}});
    r.add({.id = "midi.noteBypass", .label = "NOTE BYPASS", .shortLabel = "BYPASS", .group = "NOTE",
           .type = ParamType::Bool, .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = offOnLabels,
           .scope = ParamScope::Track, .storage = {.key = "midi.note.bypass", .reference = "bypass"}});
    // CC1 module (MID-041): param 1 pitch bend, param 2 aftertouch (channel pressure), params 3–6 CCs.
    r.add({.id = "midi.pitchBend", .label = "PITCH BEND", .shortLabel = "P.BEND", .group = "CC1",
           .type = ParamType::Integer, .minValue = -8192, .maxValue = 8191, .defaultValue = 0, .coarseStep = 64,
           .fineStep = 1, .bipolar = true, .scope = ParamScope::Track, .storage = {.reference = "pitchBend"}});
    r.add({.id = "midi.pitchBendActive", .label = "ACTIVE 1", .shortLabel = "ACTIVE 1", .group = "CC1",
           .type = ParamType::Bool, .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = offOnLabels,
           .scope = ParamScope::Track});
    r.add({.id = "midi.aftertouch", .label = "AFTER TOUCH", .shortLabel = "A.TOUCH", .group = "CC1",
           .type = ParamType::Integer, .minValue = 0, .maxValue = 127, .defaultValue = 0, .scope = ParamScope::Track,
           .storage = {.reference = "afterTouch"}});
    r.add({.id = "midi.aftertouchActive", .label = "ACTIVE 2", .shortLabel = "ACTIVE 2", .group = "CC1",
           .type = ParamType::Bool, .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = offOnLabels,
           .scope = ParamScope::Track});
    for (int i = 0; i < 4; ++i)
    {
        r.add({.id = kCc1Num[i], .label = kCcLabel[i + 2], .shortLabel = kCcLabel[i + 2], .group = "CC1",
               .type = ParamType::Integer, .minValue = 0, .maxValue = 127, .defaultValue = kCc1Defaults[i],
               .scope = ParamScope::Track});
        r.add({.id = kCc1Val[i], .label = kParamLabel[i + 2], .shortLabel = kParamShort[i + 2], .group = "CC1",
               .type = ParamType::Integer, .minValue = 0, .maxValue = 127, .defaultValue = 0,
               .scope = ParamScope::Track});
        r.add({.id = kCc1Act[i], .label = kActLabel[i + 2], .shortLabel = kActLabel[i + 2], .group = "CC1",
               .type = ParamType::Bool, .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = offOnLabels,
               .scope = ParamScope::Track});
    }
    r.add({.id = "midi.cc1Bypass", .label = "CC1 BYPASS", .shortLabel = "BYPASS", .group = "CC1",
           .type = ParamType::Bool, .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = offOnLabels,
           .scope = ParamScope::Track, .storage = {.key = "midi.cc1.bypass"}});
    for (int i = 0; i < 6; ++i)
    {
        r.add({.id = kCc2Num[i], .label = kCcLabel[i], .shortLabel = kCcLabel[i], .group = "CC2",
               .type = ParamType::Integer, .minValue = 0, .maxValue = 127, .defaultValue = kCc2Defaults[i],
               .scope = ParamScope::Track});
        r.add({.id = kCc2Val[i], .label = kParamLabel[i], .shortLabel = kParamShort[i], .group = "CC2",
               .type = ParamType::Integer, .minValue = 0, .maxValue = 127, .defaultValue = 0,
               .scope = ParamScope::Track});
        r.add({.id = kCc2Act[i], .label = kActLabel[i], .shortLabel = kActLabel[i], .group = "CC2",
               .type = ParamType::Bool, .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = offOnLabels,
               .scope = ParamScope::Track});
    }
    r.add({.id = "midi.cc2Bypass", .label = "CC2 BYPASS", .shortLabel = "BYPASS", .group = "CC2",
           .type = ParamType::Bool, .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = offOnLabels,
           .scope = ParamScope::Track, .storage = {.key = "midi.cc2.bypass"}});
    // MID-050: Bank Select MSB / LSB and Program Change per scene and MIDI track (MidiOtherData), −1 = OFF (not sent).
    r.add({.id = "midi.bankMsb", .label = "BANK MSB", .shortLabel = "MSB", .group = "BANK SELECT",
           .type = ParamType::Integer, .minValue = -1, .maxValue = 127, .defaultValue = -1, .format = formatOffOr,
           .scope = ParamScope::Track, .storage = {.reference = "bankselmsb"}});
    r.add({.id = "midi.bankLsb", .label = "BANK LSB", .shortLabel = "LSB", .group = "BANK SELECT",
           .type = ParamType::Integer, .minValue = -1, .maxValue = 127, .defaultValue = -1, .format = formatOffOr,
           .scope = ParamScope::Track, .storage = {.reference = "banksellsb"}});
    r.add({.id = "midi.program", .label = "PROG CHANGE", .shortLabel = "PROGRAM", .group = "PROG CHANGE",
           .type = ParamType::Integer, .minValue = -1, .maxValue = 127, .defaultValue = -1, .format = formatOffOr,
           .scope = ParamScope::Track, .storage = {.reference = "programchange"}});
    r.add({.id = "midi.mode", .label = "MIDI MODE", .shortLabel = "MODE", .group = "NOTE", .type = ParamType::Enum,
           .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = midiModeLabels, .scope = ParamScope::Track,
           .storage = {.reference = "midimode"}});
}
} // namespace
} // namespace core

APP_REGISTER_PARAMS("midi", 1010, &core::addMidiParams)
