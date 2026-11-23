// Track, pad and scale settings the trigger router and the track sources read (spec 02-features/tracks.md TRK-001…020,
// pads-and-scales.md PAD-001…013): one TrackSettings per [scene][track] (a scene switch is an index change, never a
// copy) and the pad routing state of the whole surface. Filled from the FIFO drain (ParamChange) with applyTrackParam /
// applyPadParam; real-time safe, no allocation.
//
// Wire units (ParamChange::value = the ParamSpec units of core/params/catalogue/track.cpp):
//   track.mode         0 SAMPLE, 1 MIDI, 2 THRU, 3 INSTRUMENT (core::model::TrackMode)
//   track.triggerMode  0 ONE SHOT, 1 GATE (core::model::TriggerMode)
//   track.chokeGroup   0 = none ("--"), 1–8
//   track.inputSource  0 STEREO, 1 L, 2 R, 3 MONO (Thru tracks; the sampling input stage owns the input)
//   scale.type         0–13 (core::scales::kScaleMasks order), scale.keyShift −12…+12, scale.area −2…+2
//   scaleMidi.type     0–13, scaleMidi.root 0–127, scaleMidi.area −2…+2: SET SCALE (MIDI), the SCALE pads of a MIDI
//                      track (PAD-020); with NOTE 1 (midi.note1, set by the MIDI engine: setMidiNote1) they give the
//                      pad's transposition of the track's notes
//   pads.mode          0 PLAY (no key lit), 1 TRACK, 2 MUTE, 3 SLICE, 4 SCALE (PAD-001), project scope 0xFFFF
//   pads.selected      the selected track 0–15 (TRK-041), pads.shift 0/1 (SHIFT held: pads select, PAD-004)
#pragma once

#include "core/model/enums/Enums.h"
#include "core/util/scales/Scales.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace engine::trigger
{
using core::model::TrackMode;
using core::model::TriggerMode;

enum class PadMode : std::uint8_t { Play, Track, Mute, Slice, Scale }; // = ui::input::pads::PadMode, pads.mode
enum class InputSource : std::uint8_t { Stereo, Left, Right, Mono };

inline constexpr std::array<std::int8_t, core::scales::kPads> kChromaticPads =
    core::scales::padTable(core::scales::kScaleMasks[0], 0, 0);

struct TrackSettings // per [scene][track]
{
    TrackMode mode = TrackMode::Sample;
    TriggerMode triggerMode = TriggerMode::OneShot;
    std::uint8_t chokeGroup = 0; // 0 none, 1–8
    InputSource input = InputSource::Stereo;
    std::uint8_t scaleType = 0;
    std::int8_t keyShift = 0, window = 0;
    std::array<std::int8_t, core::scales::kPads> padSemitones = kChromaticPads; // derived from the three above
    // SET SCALE (MIDI) (PAD-020) and the track's NOTE 1: a SCALE pad of a MIDI track plays core::scales::midiPadNote.
    std::uint8_t midiScaleType = 0, midiRoot = 60;
    std::int8_t midiWindow = 0, midiNote1 = 60;
    std::array<std::int8_t, core::scales::kPads> midiPadSemitones = kChromaticPads; // degrees from the root
};

// The semitones SCALE pad `pad` adds to track `s`'s pitch (Sample tracks: SET SCALE) or to its MIDI notes (MIDI
// tracks: NOTE 1 becomes the pad's note of SET SCALE (MIDI), the harmony notes keep their intervals).
inline int scalePadSemitones(const TrackSettings& s, int pad) noexcept
{
    const auto p = static_cast<std::size_t>(pad & (core::scales::kPads - 1));
    if (s.mode != TrackMode::Midi)
        return s.padSemitones[p];
    return core::scales::clampInt(s.midiRoot + s.midiPadSemitones[p], 0, 127) - s.midiNote1;
}

struct PadState // the surface (one for all scenes)
{
    PadMode mode = PadMode::Play;
    std::uint8_t selected = 0; // track 0–15
    bool shift = false;
};

enum class TrackField : std::uint8_t
{
    Mode, TriggerMode, ChokeGroup, InputSource, ScaleType, KeyShift, ScaleArea, MidiScaleType, MidiScaleRoot,
    MidiScaleArea, Count
};
enum class PadField : std::uint8_t { Mode, Selected, Shift, Count };

struct TrackParamId
{
    std::string_view id;
    TrackField field;
};
struct PadParamId
{
    std::string_view id;
    PadField field;
};
// Set-up only (string ids never reach the audio thread). Every other id of track.cpp (padColor, velocity curve …) is
// message-thread state the engine does not implement.
inline constexpr std::array<TrackParamId, static_cast<std::size_t>(TrackField::Count)> kTrackParamIds {{
    {"track.mode", TrackField::Mode},
    {"track.triggerMode", TrackField::TriggerMode},
    {"track.chokeGroup", TrackField::ChokeGroup},
    {"track.inputSource", TrackField::InputSource},
    {"scale.type", TrackField::ScaleType},
    {"scale.keyShift", TrackField::KeyShift},
    {"scale.area", TrackField::ScaleArea},
    {"scaleMidi.type", TrackField::MidiScaleType},
    {"scaleMidi.root", TrackField::MidiScaleRoot},
    {"scaleMidi.area", TrackField::MidiScaleArea},
}};
inline constexpr std::array<PadParamId, static_cast<std::size_t>(PadField::Count)> kPadParamIds {{
    {"pads.mode", PadField::Mode},
    {"pads.selected", PadField::Selected},
    {"pads.shift", PadField::Shift},
}};

// Lock slot of TRIGGER MODE (core::ParamIndex 9, TRK-013).
constexpr int kLockSlotTriggerMode = 9;

// One ParamChange value into the block; recomputes the pad table on a scale change. False when nothing changed.
bool applyTrackParam(TrackSettings&, TrackField, float value) noexcept;
bool applyPadParam(PadState&, PadField, float value) noexcept;
} // namespace engine::trigger
