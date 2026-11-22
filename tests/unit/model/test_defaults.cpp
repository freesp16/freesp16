// makeDefaultProject() (PRJ-002 and the manual defaults the spec names) validates, and the entity resets restore them.
#include "core/model/ModelOps.h"
#include "core/model/enums/EnumStrings.h"
#include "core/model/enums/ValueLists.h"

#include <catch2/catch_test_macros.hpp>

using namespace core::model;

TEST_CASE("the default project carries the spec's defaults", "[model]")
{
    auto p = newDefaultProject();
    // PRJ-002
    CHECK(p->settings.tempo == 120.0);
    CHECK(p->settings.currentScene == 0);
    CHECK(p->settings.currentPattern == 0);
    CHECK(p->settings.patternQuantize == QuantizeUnit::Pattern); // M p.42
    CHECK(p->settings.padQuantize == QuantizeUnit::Step);        // 1/16* (SEQ-042)
    CHECK(p->settings.swing == kMinSwing);                       // straight
    CHECK(p->settings.monitorVolume == 100);                     // M p.33
    CHECK(p->ribbon.pitchTopCents == 200);                       // RIB-002
    CHECK(p->ribbon.pitchBottomCents == -200);
    CHECK(p->arrangement.sections.empty());
    CHECK_FALSE(p->settings.playArrangement);
    for (int s = 0; s < kScenes; ++s)
    {
        const auto& scene = p->scenes[static_cast<std::size_t>(s)];
        CHECK(scene.colour == s); // SCN-003
        CHECK_FALSE(scene.bpm.has_value());
        CHECK(scene.sendFx.type == EffectType::Off);
        CHECK(scene.masterFx.type == EffectType::Off);
        CHECK(scene.routing.fxMode == FxMode::Both);
        CHECK(scene.routing.assignMode == AssignMode::MasterPlus6);
        for (const auto& pattern : scene.patterns)
            CHECK(pattern.length == 16);
        for (int t = 0; t < kTracks; ++t)
        {
            const auto& tr = scene.tracks[static_cast<std::size_t>(t)];
            CHECK(tr.mode == TrackMode::Sample);
            CHECK(tr.playback.sample.empty());
            CHECK(tr.colour == t); // TRK-031
            CHECK(tr.insertFx[0].type == EffectType::Off);
            CHECK(tr.insertFx[1].type == EffectType::Off);
            CHECK(tr.chokeGroup == 0);
            CHECK(tr.routing.output == OutputAssign::Master);
        }
    }
    const auto& t = p->scenes[5].tracks[9];
    // ENV-011
    CHECK(t.ampEnv.attack == 0.0);
    CHECK(t.ampEnv.hold == 1.0); // INFINITY
    CHECK(t.ampEnv.release == 0.0);
    CHECK_FALSE(t.ampEnv.sync);
    CHECK(t.ampEnv.volumeIndex == 127);
    CHECK(t.ampEnv.attackCurve == EnvelopeCurve::Linear);
    // SEQ-030, SHIFT AREA
    CHECK(t.seq.retriggerCount == 1);
    CHECK(enumFromInt<RetriggerSpeed>(t.seq.retriggerSpeed) == RetriggerSpeed::Quarter);
    CHECK(enumFromInt<StepShiftArea>(t.seq.stepShiftArea) == StepShiftArea::All);
    // LFO
    CHECK(t.lfo.shape == LfoShape::Sine);
    CHECK(t.lfo.depth == 63);
    CHECK(t.lfo.triggerMode == LfoTriggerMode::Retrigger);
    // MIDI track
    CHECK(t.midi.notes[0] == 60); // C5
    CHECK(t.midi.notes[1] == -1);
    CHECK(t.midi.velocity == 100);
    CHECK(t.midi.length == 1.0);
    CHECK(t.midi.cc1[0].cc == 7);
    CHECK(t.midi.cc2[5].cc == 76);
    CHECK(enumFromInt<MidiTrackMode>(t.midi.mode) == MidiTrackMode::Notes);
    CHECK(p->midi.input.channels[9] == 10); // track n → channel n
    CHECK(enumFromInt<ScaleType>(t.padScale.scaleType) == ScaleType::Chromatic);
    CHECK(t.slices.gridCount == 16);
}

TEST_CASE("makeDefaultProject() validates and equals the heap and in-place forms", "[model]")
{
    auto heap = newDefaultProject();
    std::vector<std::string> problems;
    CHECK(validate(*heap, problems));
    for (const auto& line : problems)
        UNSCOPED_INFO(line);
    CHECK(problems.empty());

    auto byValue = std::make_unique<Project>(makeDefaultProject());
    CHECK(*byValue == *heap);

    auto dirty = std::make_unique<Project>(*heap);
    dirty->settings.tempo = 99.0;
    dirty->scenes[2].name = "VERSE";
    dirty->scenes[2].bpm = 140.0;
    dirty->scenes[2].tracks[3].volume = 7;
    dirty->scenes[2].patterns[1].steps[4][3].trigger = 1;
    dirty->scenes[2].patterns[1].steps[4][3].lockIndex = 0;
    dirty->scenes[2].patterns[1].locks.push_back({});
    dirty->sampleRefs.entries.push_back({"abc", "$Lib/kick.wav", "KICK", false, {}});
    dirty->arrangement.sections.push_back({});
    resetToDefaults(*dirty);
    CHECK(*dirty == *heap);
}

TEST_CASE("entity resets restore the defaults of their index", "[model]")
{
    auto p = newDefaultProject();
    const auto reference = std::make_unique<Project>(*p);

    auto& track = p->scenes[1].tracks[4];
    track.volume = 3;
    track.mode = TrackMode::Midi;
    track.insertFx[0].type = EffectType::Chorus;
    resetTrack(track, 4);
    CHECK(track == reference->scenes[1].tracks[4]);
    CHECK(defaultTrack(4) == reference->scenes[1].tracks[4]);

    auto& pattern = p->scenes[1].patterns[2];
    pattern.length = 64;
    pattern.trackMutes[3] = true;
    pattern.steps[10][3].trigger = 5;
    pattern.locks.resize(3);
    resetPattern(pattern);
    CHECK(pattern == reference->scenes[1].patterns[2]);
    CHECK(defaultPattern() == reference->scenes[1].patterns[2]);

    auto& scene = p->scenes[7];
    scene.name = "X";
    scene.colour = 0;
    scene.routing.fxMode = FxMode::Send;
    scene.sendFx.type = EffectType::Reverb;
    scene.tracks[15].pan = 20;
    resetScene(scene, 7);
    CHECK(scene == reference->scenes[7]);
    CHECK(*p == *reference);
}

namespace core::model::detail
{
std::unique_ptr<Project> allocateProject(); // core/model/ProjectStorage.cpp (GCC 11 adapter)
}

TEST_CASE("the GCC 11 construction adapter yields a default-constructed Project", "[model]")
{
    auto adapted = detail::allocateProject();
    auto plain = std::make_unique<Project>();
    CHECK(*adapted == *plain);
}
