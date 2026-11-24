// One track of one scene (data-model.md "Track" and its parts; tracks.md TRK-*, playback PLY-*, envelope ENV-*,
// LFO-*, MIDI tracks, routing MIX-031). Plain value types with the defaults a default-constructed project needs; the
// manual defaults the spec cites are applied by makeDefaultProject().
#pragma once

#include "core/model/EffectSlot.h"
#include "core/model/Extras.h"
#include "core/model/Sample.h"
#include "core/model/enums/Enums.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace core::model
{
struct Playback // PLY
{
    std::int64_t start = 0, loopStart = 0, length = 0; // sample frames
    bool loop = false;
    bool reverse = false;
    int pitchCents = 0;
    StretchMode stretchMode = StretchMode::Off;
    SampleKey sample {};
    bool bypass = false;
    Extras extras {};
    friend bool operator== (const Playback&, const Playback&) = default;
};

struct AmpEnvelope // ENV
{
    double attack = 0, hold = 1, release = 0; // normalised 0–1 (hold/release INFINITY = 1.0, ENV-002)
    int volumeIndex = 100;
    bool sync = false;
    bool velocitySensitive = true;
    bool retrigger = false;
    EnvelopeCurve attackCurve = EnvelopeCurve::Linear, releaseCurve = EnvelopeCurve::Linear;
    bool bypass = false;
    Extras extras {};
    friend bool operator== (const AmpEnvelope&, const AmpEnvelope&) = default;
};

struct TrackSequencerSettings // SEQ (retrigger, offset, step shift, transpose)
{
    int retriggerCount = 0, retriggerSpeed = 0, offset = 0, stepShiftArea = 0, transpose = 0;
    Extras extras {};
    friend bool operator== (const TrackSequencerSettings&, const TrackSequencerSettings&) = default;
};

struct SliceSettings // SLC; user grid layout unknown in the reference (OQ-04): kept opaque in extras by the importer
{
    std::vector<std::int64_t> grids; // user-moved slice points (frames)
    int gridCount = 16;
    int windowOffset = 0;
    Extras extras {};
    friend bool operator== (const SliceSettings&, const SliceSettings&) = default;
};

struct PadScale // ScaleSetting (sample tracks)
{
    int scaleType = 0, keyShift = 0, windowOffset = 0;
    Extras extras {};
    friend bool operator== (const PadScale&, const PadScale&) = default;
};

struct MidiScale // ScaleSettingMidi (MIDI tracks)
{
    int scaleTypeMidi = 0, scalePosition = 0, rootNote = 60;
    Extras extras {};
    friend bool operator== (const MidiScale&, const MidiScale&) = default;
};

struct Send
{
    int channel = 0;  // send destination
    int amount = 0;   // 0–127
    Extras extras {};
    friend bool operator== (const Send&, const Send&) = default;
};

struct Lfo // LFO-*
{
    double speed = 0;
    int depth = 0, phaseOffset = 0;
    int destination = 0; // reference id: 100–155 FX1 params, 200–255 FX2 params [DSP]; see catalogue/lfo.cpp
    LfoShape shape = LfoShape::Sine;
    LfoTriggerMode triggerMode = LfoTriggerMode::Free;
    LfoTriggerOn triggerOn = LfoTriggerOn::Pad;
    bool sync = false;
    int randomSeedCount = 0;
    bool bypass = false;
    Extras extras {};
    friend bool operator== (const Lfo&, const Lfo&) = default;
};

struct MidiCcParam
{
    int cc = 0, value = 0;
    bool active = false;
    friend bool operator== (const MidiCcParam&, const MidiCcParam&) = default;
};

struct MidiTrack // MIDI track settings (NOTE, CC1, CC2, bank/program, mode)
{
    std::array<int, 4> notes {60, 0, 0, 0};    // NOTE 0–127, then NOTE2–4 as semitones from it (−127…127, 0 = off)
    int velocity = 100;                         // 0 = OFF, 1–127
    double length = 1.0;                        // steps, 0.25–64
    bool noteBypass = false;
    int pitchBendValue = 0;
    bool pitchBendActive = false;
    int aftertouchValue = 0;
    bool aftertouchActive = false;
    std::array<MidiCcParam, 4> cc1 {{{7, 0, false}, {1, 0, false}, {2, 0, false}, {10, 0, false}}}; // CC1 params 3–6
    bool cc1Bypass = false;
    std::array<MidiCcParam, 6> cc2 {{{71, 0, false}, {72, 0, false}, {73, 0, false}, {74, 0, false}, {75, 0, false},
                                     {76, 0, false}}};
    bool cc2Bypass = false;
    int bankMsb = -1, bankLsb = -1, program = -1; // -1 = not sent
    int mode = 0;
    Extras extras {};
    friend bool operator== (const MidiTrack&, const MidiTrack&) = default;
};

struct TrackRouting // MIX-031
{
    OutputAssign output = OutputAssign::Master;
    Extras extras {};
    friend bool operator== (const TrackRouting&, const TrackRouting&) = default;
};

struct Track
{
    TrackMode mode = TrackMode::Sample;
    TriggerMode triggerMode = TriggerMode::OneShot;
    int chokeGroup = 0; // 0 = none, 1–8
    int volume = 100;   // 0–127 (GainLaw)
    int pan = 0;        // −64…+64
    bool mute = false, solo = false;
    int colour = 0;     // palette index
    int instrument = 0; // 0 = Off, else an instrument profile id
    std::string name {};
    Playback playback {};
    AmpEnvelope ampEnv {};
    TrackSequencerSettings seq {};
    SliceSettings slices {};
    PadScale padScale {};
    MidiScale midiScale {};
    std::array<EffectSlot, 2> insertFx {};
    Send send {};
    Lfo lfo {};
    MidiTrack midi {};
    PluginTrack plugin {};
    TrackRouting routing {};
    Extras extras {};
    friend bool operator== (const Track&, const Track&) = default;
};
} // namespace core::model
