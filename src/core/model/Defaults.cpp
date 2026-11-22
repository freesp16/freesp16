// Manual defaults of the document model. Only the values that differ from the header defaults
// are set here; each line cites where the spec names the value.
#include "core/model/ModelOps.h"
#include "core/model/enums/ValueLists.h"

namespace core::model
{
namespace detail
{
std::unique_ptr<Project> allocateProject(); // ProjectStorage.cpp: the one place a Project is constructed (GCC 11)
} // namespace detail

namespace
{
// Applied on top of a default-constructed Track.
void applyTrackDefaults(Track& t, int trackIndex)
{
    t.colour = trackIndex % kPaletteColours;    // TRK-031, enumerations.md "Colours": track n → index n − 1
    t.ampEnv.volumeIndex = 127;                 // ENV-011: VELOCITY 127; 1.60 default 127 [OBS]
    t.seq.retriggerCount = 1;                   // SEQ-030: RETRIGGER CNT 1 (a single trigger) [MAN]
    t.seq.retriggerSpeed = static_cast<int>(RetriggerSpeed::Quarter); // SEQ-030: 1/4 step [MAN] (M p.40)
    t.seq.stepShiftArea = static_cast<int>(StepShiftArea::All);       // SHIFT AREA ALL* [OBS]
    t.lfo.depth = 63;                           // enumerations.md LFO depth: 1.60 default 63 [OBS]
    t.lfo.triggerMode = LfoTriggerMode::Retrigger; // LFO SETTING Mode: 1.60 default `Trigger` [OBS]
    // Header defaults already match the spec: mode Sample (TRK-001), OneShot (1.60), choke "--", volume 100 (0 dB,
    // MIX-001), pan C, attack 0 / hold INFINITY / release 0 / sync off / LINEAR curves (ENV-011, M p.37–38), pitch 0
    // (PLY-020), stretch OFF (empty track), grid count 16 (SLC-001), scale CHROMATIC, root C5 = 60, MIDI note C5 with
    // harmony OFF, velocity 100, length 1 step, CC1 7/1/2/10 and CC2 71–76, bank/program OFF, NOTES mode, both
    // insert FX Off (PRJ-002), OUT ASSIGN "--" (M + 6CH default).
}

void applySceneDefaults(Scene& s, int sceneIndex)
{
    s.colour = sceneIndex % kPaletteColours; // SCN-003: scene n → palette colour n
    for (int t = 0; t < kTracks; ++t)
        applyTrackDefaults(s.tracks[static_cast<std::size_t>(t)], t);
    // Patterns: length 16 (PRJ-002), all cells empty. Routing: M + 6CH, HP CUE MASTER, FX mode BOTH (FX-009),
    // no AFS track. Send / Master FX Off (PRJ-002). BPM absent = follow the project tempo.
}

void applyProjectDefaults(Project& p)
{
    p.settings.monitorVolume = 100;                  // MONITOR LEVEL default 100 [MAN] (M p.33)
    p.settings.padQuantize = QuantizeUnit::Step;     // record QUANTIZE 1/16* [OBS] (SEQ-042)
    p.ribbon.pitchTopCents = 200;                    // RIB-002: default −200…+200 cents [MAN] (M p.15)
    p.ribbon.pitchBottomCents = -200;
    for (int s = 0; s < kScenes; ++s)
        applySceneDefaults(p.scenes[static_cast<std::size_t>(s)], s);
    // Header defaults already match: tempo 120.0 (PRJ-002), swing straight (stored 50), PATTERN QUANTIZE PATTERN
    // (M p.42), soft limiter off, arrangement off and empty, scene 1 / pattern 1 current, format version 1.
}
} // namespace

Project makeDefaultProject()
{
    auto p = newDefaultProject();
    return std::move(*p); // the move constructor is cheap to compile; constructing is not (ProjectStorage.cpp)
}

std::unique_ptr<Project> newDefaultProject()
{
    auto p = detail::allocateProject();
    applyProjectDefaults(*p);
    return p;
}

Track defaultTrack(int trackIndex)
{
    Track t;
    applyTrackDefaults(t, trackIndex);
    return t;
}

Pattern defaultPattern() { return Pattern {}; }

void resetTrack(Track& t, int trackIndex)
{
    t = Track {};
    applyTrackDefaults(t, trackIndex);
}

void resetPattern(Pattern& p)
{
    p.length = Pattern {}.length;
    p.trackMutes.fill(false);
    p.trackSolos.fill(false);
    for (auto& row : p.steps)
        row.fill(Step {});
    p.locks.clear();
    p.locks.shrink_to_fit();
    p.extras = {};
}

void resetScene(Scene& s, int sceneIndex)
{
    s.name.clear();
    s.bpm.reset();
    for (auto& t : s.tracks)
        t = Track {};
    for (auto& pattern : s.patterns)
        resetPattern(pattern);
    s.sendFx = {};
    s.masterFx = {};
    s.routing = {};
    s.extras = {};
    applySceneDefaults(s, sceneIndex);
}

void resetToDefaults(Project& p)
{
    p.settings = {};
    for (int s = 0; s < kScenes; ++s)
    {
        auto& scene = p.scenes[static_cast<std::size_t>(s)];
        scene.name.clear();
        scene.bpm.reset();
        for (auto& t : scene.tracks)
            t = Track {};
        for (auto& pattern : scene.patterns)
            resetPattern(pattern);
        scene.sendFx = {};
        scene.masterFx = {};
        scene.routing = {};
        scene.extras = {};
    }
    p.arrangement = {};
    p.ribbon = {};
    p.midi = {};
    p.sampleRefs = {};
    p.extras = {};
    applyProjectDefaults(p);
}
} // namespace core::model
