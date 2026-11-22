// Parameter catalogue, area TRACK (TRK) + pads and scales (PAD): the track settings of TRACK MENU / TRACK SETTING /
// HOME, the pad response preferences, the scales of Sample and MIDI tracks, and the pad routing state the engine's
// trigger router needs for pad hits that reach it straight from the reader thread (UI-118). One file per area
// (UID-200/201). track.volume / track.pan / mute / solo live in mixer.cpp.
//
// Spec: 02-features/tracks.md TRK-001…041, pads-and-scales.md PAD-001…020, 03-ui/control-surface.md UI-114,
// 07-ui-design/screens/playback.md "SET SCALE". Values are the engine's wire units (engine/trigger/TrackParams.h).
#include "core/params/ParamRegistry.h"

#include "core/util/scales/Scales.h"

#include <cstdio>
#include <iterator>

namespace core
{
namespace
{
constexpr std::string_view trackModeLabels[] = {"SAMPLE", "MIDI", "THRU", "INSTRUMENT"}; // core::model::TrackMode
constexpr std::string_view triggerLabels[] = {"ONE SHOT", "GATE"};
constexpr std::string_view chokeLabels[] = {"--", "1", "2", "3", "4", "5", "6", "7", "8"};
constexpr std::string_view colourLabels[] = {"RED",    "VERMILION", "ORANGE", "AMBER",    "GOLD",       "YELLOW",
                                             "LIME",   "GREEN",     "JADE",   "CYAN",     "SKY BLUE",   "PERIWINKLE",
                                             "VIOLET", "ORCHID",    "PINK",   "ROSE"}; // model::PaletteColour
constexpr std::string_view offOnLabels[] = {"OFF", "ON"};
constexpr std::string_view inputLabels[] = {"STEREO", "L", "R", "MONO"};
constexpr std::string_view curveLabels[] = {"HARD 3", "HARD 2", "HARD 1", "LINEAR", "SOFT 1", "SOFT 2", "SOFT 3", "FIXED"};
constexpr std::string_view scaleLabels[] = {"CHROMATIC",   "MAJOR",       "MINOR",    "HARMONIC MINOR", "MELODIC MINOR",
                                            "DORIAN",      "PHRYGIAN",    "LYDIAN",   "MIXOLYDIAN",     "LOCRIAN",
                                            "MAJOR PENTA", "MINOR PENTA", "BLUES",    "WHOLE TONE"}; // PAD-010, OQ-10
constexpr std::string_view padModeLabels[] = {"OFF", "TRACK", "MUTE", "SLICE", "SCALE"}; // PAD-001 (OFF = play)
static_assert(std::size(scaleLabels) == scales::kScales);

int formatTrackNumber(float v, char* buf, int n) noexcept // 0–15 shown 1–16
{
    return std::snprintf(buf, static_cast<std::size_t>(n), "%d", static_cast<int>(v + 0.5f) + 1);
}

int formatSigned(float v, char* buf, int n) noexcept
{
    const int i = static_cast<int>(v < 0 ? v - 0.5f : v + 0.5f);
    return std::snprintf(buf, static_cast<std::size_t>(n), i > 0 ? "+%d" : "%d", i);
}

int formatNote(float v, char* buf, int n) noexcept // MIDI note name, C5 = 60 (UI-133 convention)
{
    static constexpr const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const int note = static_cast<int>(v + 0.5f);
    return std::snprintf(buf, static_cast<std::size_t>(n), "%s%d", names[(note % 12 + 12) % 12], note / 12);
}

void addTrackParams(ParamRegistry& r)
{
    // TRK-001/002: track type, changed only in TRACK MENU (TRACK TYPE), undoable (core/commands/track setTrackMode).
    r.add({.id = "track.mode", .label = "TRACK TYPE", .shortLabel = "TYPE", .group = "TRACK", .type = ParamType::Enum,
           .minValue = 0, .maxValue = 3, .defaultValue = 0, .enumLabels = trackModeLabels, .scope = ParamScope::Track,
           .storage = {.reference = "trackMode"}});
    // TRK-010…014: ONE SHOT / GATE, CC 16, lockable (slot 9).
    r.add({.id = "track.triggerMode", .label = "TRIGGER MODE", .shortLabel = "TRIGGER", .group = "TRACK",
           .type = ParamType::Enum, .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = triggerLabels,
           .lockable = true, .midiCc = 16, .scope = ParamScope::Track, .storage = {.reference = "triggerMode"}});
    // TRK-020: CHOKE "--" (0) or 1–8.
    r.add({.id = "track.chokeGroup", .label = "CHOKE", .shortLabel = "CHOKE", .group = "TRACK", .type = ParamType::Enum,
           .minValue = 0, .maxValue = 8, .defaultValue = 0, .enumLabels = chokeLabels, .scope = ParamScope::Track,
           .storage = {.reference = "chokeGroup"}});
    // TRK-031: PAD COLOR, one of the 16 palette colours; the per-track default (track n → colour n − 1) is
    // makeDefaultProject's: a ParamSpec has one default.
    r.add({.id = "track.padColor", .label = "PAD COLOR", .shortLabel = "COLOR", .group = "TRACK",
           .type = ParamType::Enum, .minValue = 0, .maxValue = 15, .defaultValue = 0, .enumLabels = colourLabels,
           .scope = ParamScope::Track, .storage = {.reference = "colourIndex"}});
    // TRK-032: pad VELOCITY ON/OFF = the amp envelope's velocity sensitivity (ENV-006; the engine binds it to
    // AmpEnvField::VelocitySensitive).
    r.add({.id = "track.padVelocity", .label = "VELOCITY", .shortLabel = "VEL", .group = "TRACK",
           .type = ParamType::Enum, .minValue = 0, .maxValue = 1, .defaultValue = 1, .enumLabels = offOnLabels,
           .scope = ParamScope::Track, .storage = {.reference = "bVelocityEffective"}});
    // MOTION REC per track (not in the reference), OFF by default (a new project and every imported one): ON = a knob
    // turn on a lockable parameter of this track while recording writes motion (locks) on the steps the playhead passes
    // (SEQ-023); OFF = the knob only moves the track's value. Saved in the track's extras ("native.motionRec").
    r.add({.id = "track.motionRec", .label = "MOTION REC", .shortLabel = "MOTION", .group = "TRACK",
           .type = ParamType::Enum, .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = offOnLabels,
           .scope = ParamScope::Track});
    // TRK-005: THRU input source (the routing input stage consumes it; the Thru source maps it meanwhile).
    r.add({.id = "track.inputSource", .label = "INPUT SOURCE", .shortLabel = "INPUT", .group = "TRACK",
           .type = ParamType::Enum, .minValue = 0, .maxValue = 3, .defaultValue = 0, .enumLabels = inputLabels,
           .scope = ParamScope::Track});

    // PAD-002 / UI-114: pad response preferences (per controller in P2; one global value here).
    // Requested default: PAD VELOCITY OFF = every hit at full velocity (127), whatever the strength;
    // ON = the strength through PAD CURVE (FIXED: FIXED VELOCITY).
    r.add({.id = "pads.velocity", .label = "PAD VELOCITY", .shortLabel = "PAD VEL", .group = "PADS",
           .type = ParamType::Enum, .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = offOnLabels,
           .scope = ParamScope::Global, .storage = {.key = "prefs.pads.velocity"}});
    r.add({.id = "pads.velocityCurve", .label = "PAD CURVE", .shortLabel = "CURVE", .group = "PADS",
           .type = ParamType::Enum, .minValue = 0, .maxValue = 7, .defaultValue = 3, .enumLabels = curveLabels,
           .scope = ParamScope::Global, .storage = {.key = "prefs.pads.velocityCurve"}});
    r.add({.id = "pads.fixedVelocity", .label = "FIXED VELOCITY", .shortLabel = "FIX VEL", .group = "PADS",
           .type = ParamType::Integer, .minValue = 1, .maxValue = 127, .defaultValue = 100, .scope = ParamScope::Global,
           .storage = {.key = "prefs.pads.fixedVelocity"}});

    // PAD-010…013: SET SCALE (Sample tracks).
    r.add({.id = "scale.type", .label = "SCALE", .shortLabel = "SCALE", .group = "SCALE", .type = ParamType::Enum,
           .minValue = 0, .maxValue = scales::kScales - 1, .defaultValue = 0, .enumLabels = scaleLabels,
           .scope = ParamScope::Track, .storage = {.reference = "scaleType"}});
    r.add({.id = "scale.keyShift", .label = "KEY SHIFT", .shortLabel = "KEYSHIFT", .group = "SCALE",
           .type = ParamType::Integer, .minValue = scales::kMinKeyShift, .maxValue = scales::kMaxKeyShift,
           .defaultValue = 0, .format = formatSigned, .bipolar = true, .scope = ParamScope::Track,
           .storage = {.reference = "keyShift"}});
    r.add({.id = "scale.area", .label = "AREA", .shortLabel = "AREA", .group = "SCALE", .type = ParamType::Integer,
           .minValue = scales::kMinWindow, .maxValue = scales::kMaxWindow, .defaultValue = 0, .format = formatSigned,
           .bipolar = true, .scope = ParamScope::Track, .storage = {.reference = "windowOffset"}});
    // PAD-020: SET SCALE (MIDI tracks): scale, root note, scale position (paging).
    r.add({.id = "scaleMidi.type", .label = "SCALE", .shortLabel = "SCALE", .group = "SCALE MIDI",
           .type = ParamType::Enum, .minValue = 0, .maxValue = scales::kScales - 1, .defaultValue = 0,
           .enumLabels = scaleLabels, .scope = ParamScope::Track, .storage = {.reference = "scaleTypeMidi"}});
    r.add({.id = "scaleMidi.root", .label = "ROOT", .shortLabel = "ROOT", .group = "SCALE MIDI", .type = ParamType::Note,
           .minValue = 0, .maxValue = 127, .defaultValue = 60, .format = formatNote, .scope = ParamScope::Track,
           .storage = {.reference = "rootNote"}});
    r.add({.id = "scaleMidi.area", .label = "AREA", .shortLabel = "AREA", .group = "SCALE MIDI",
           .type = ParamType::Integer, .minValue = scales::kMinWindow, .maxValue = scales::kMaxWindow,
           .defaultValue = 0, .format = formatSigned, .bipolar = true, .scope = ParamScope::Track,
           .storage = {.reference = "scalePosition"}});

    // Pad routing state (runtime, never on a knob): the pad-mode keys (PAD-001), the selected track (TRK-041) and
    // SHIFT (PAD-004), posted by ui::input::pads::PadController so the engine routes the pad hits that reach it from
    // the reader thread. Global scope under "session." keys: the settings store must not persist them (the
    // surface starts with no mode key lit and track 1 selected).
    r.add({.id = "pads.mode", .label = "PAD MODE", .shortLabel = "PADMODE", .group = "PADS", .type = ParamType::Enum,
           .minValue = 0, .maxValue = 4, .defaultValue = 0, .enumLabels = padModeLabels, .scope = ParamScope::Global,
           .storage = {.key = "session.pads.mode"}});
    r.add({.id = "pads.selected", .label = "SELECTED TRACK", .shortLabel = "TRACK", .group = "PADS",
           .type = ParamType::Integer, .minValue = 0, .maxValue = 15, .defaultValue = 0, .format = formatTrackNumber,
           .scope = ParamScope::Global, .storage = {.key = "session.pads.selected"}});
    r.add({.id = "pads.shift", .label = "SHIFT", .shortLabel = "SHIFT", .group = "PADS", .type = ParamType::Bool,
           .minValue = 0, .maxValue = 1, .defaultValue = 0, .scope = ParamScope::Global,
           .storage = {.key = "session.pads.shift"}});
}
} // namespace
} // namespace core

APP_REGISTER_PARAMS("track", 100, &core::addTrackParams)
