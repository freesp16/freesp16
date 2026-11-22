// The encodings a real 1.6x project uses, synthesised by prj-synth's "observed" schema
// (tests/unit/interop/synthetic/observed.json — generated values only): FxType_ effect names and bType1/bType2 option
// strings, stretch codes, LFO "Full", packed hi16/lo16 ints, −1 sentinels, the observed AmpEnvelope / MIDI /
// routing / ribbon names, $Ext0/%Smp sample URLs, value2 that is no velocity, the observed lock ordinals, the empty FX
// word index 5 + k.
#include "core/commands/sequencer/SequencerCommands.h"
#include "core/model/ModelOps.h"
#include "core/model/enums/ValueLists.h"
#include "core/params/ParamRegistry.h"
#include "services/interop/sp16/Sp16Import.h"
#include "services/interop/sp16/Sp16ProjectCodec.h"
#include "tests/unit/interop/Fixtures.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace services::interop::sp16;
namespace m = core::model;
using Catch::Approx;

namespace
{
int slotOf(std::string_view id) { return core::commands::sequencer::lockSlotOf(*core::builtinParams().find(id)); }
bool has(const services::store::ImportReport& r, std::string_view text)
{
    for (const auto* list : {&r.warnings, &r.unmapped})
        for (const auto& l : *list)
            if (l.find(text) != std::string::npos)
                return true;
    return false;
}
bool anyStartsWith(const services::store::ImportReport& r, std::string_view prefix)
{
    for (const auto* list : {&r.warnings, &r.unmapped})
        for (const auto& l : *list)
            if (l.rfind(prefix, 0) == 0)
                return true;
    return false;
}
} // namespace

TEST_CASE("prj observed: the encodings of a real 1.6x project map without unknown properties", "[interop]")
{
    INTEROP_FIXTURE(g, "observed");
    services::store::ImportReport report;
    auto r = importProjectFile(g.prj, {}, report);
    REQUIRE(r);
    const auto& p = *r.value();
    INFO([&] { std::string all; for (const auto& l : report.warnings) all += l + "\n"; for (const auto& l : report.unmapped) all += l + "\n"; return all; }());

    // Nothing unknown, nothing out of range, no validate problem, no per-sample path guess.
    CHECK_FALSE(anyStartsWith(report, "unknown-prop "));
    CHECK_FALSE(anyStartsWith(report, "type-mismatch "));
    CHECK_FALSE(anyStartsWith(report, "clamped "));
    CHECK_FALSE(has(report, "validate:"));
    CHECK_FALSE(has(report, "volume $Ext0"));
    CHECK_FALSE(has(report, "path alias"));
    CHECK(has(report, "samples referenced=2 found=2 missing=0"));
    CHECK(has(report, "verbatimPatterns=0"));
    CHECK(has(report, "roundtrip tree identical"));

    // Project: packed quantise units (hi/lo note lengths), swing as the shown %.
    CHECK(p.settings.tempo == 126.0);
    CHECK(p.settings.swing == 60);
    CHECK(p.settings.patternQuantize == m::QuantizeUnit::Pattern); // 4/1
    CHECK(p.settings.padQuantize == m::QuantizeUnit::Step);        // 1/16
    CHECK(p.settings.formatVersion == 3);
    CHECK(has(report, "guessed ProjectData.padQuantizeUnit packed hi/lo 1/16"));

    const auto& s0 = p.scenes[0];
    CHECK(s0.colour == 0);                                        // −1 = none: the default colour
    CHECK(s0.extras.find("sp16.SceneData.colourIndex") != nullptr); // … the stored −1 kept for the exporter
    const auto& t0 = s0.tracks[0];
    CHECK(t0.chokeGroup == 0); // −1 = none
    CHECK(t0.playback.stretchMode == m::StretchMode::MasterTempo); // "MT"
    CHECK(t0.ampEnv.attack == Approx(10.0 / 127));
    CHECK(t0.ampEnv.release == Approx(40.0 / 127));
    CHECK(t0.ampEnv.velocitySensitive);
    CHECK(t0.ampEnv.retrigger);
    CHECK(t0.insertFx[0].type == m::EffectType::MultiModeFilter);
    CHECK(t0.insertFx[0].type1 == 1); // HPF
    CHECK(t0.insertFx[0].type2 == 1); // 24 dB/Oct
    CHECK(t0.insertFx[1].type == m::EffectType::LoFi);
    CHECK(t0.insertFx[1].type1 == 1); // LP
    CHECK(t0.insertFx[1].type2 == 0); // PRE
    CHECK_FALSE(s0.masterFx.bypass); // FX-046: the stored bBypass = true loads off, kept silently for the exporter
    CHECK(s0.masterFx.extras.find("sp16.MasterFx.bBypass") != nullptr);
    CHECK_FALSE(has(report, "MasterFx.bBypass"));
    CHECK(t0.lfo.triggerOn == m::LfoTriggerOn::Step);
    CHECK(t0.lfo.extras.find("sp16.LfoData.triggerOn")->value == "Full");
    CHECK(t0.seq.retriggerSpeed == static_cast<int>(m::RetriggerSpeed::Quarter)); // 1/4
    const auto def = m::defaultTrack(0);
    CHECK(t0.seq.stepShiftArea == def.seq.stepShiftArea);                        // −1 = none: the default
    CHECK(t0.midi.mode == def.midi.mode);                                        // −1 = none: the default
    CHECK(t0.midi.notes[1] == -1);                                               // 0 = off
    CHECK(t0.midi.length == 1.0);                                                // 4 quarter steps

    const auto& t1 = s0.tracks[1];
    CHECK(t1.playback.stretchMode == m::StretchMode::Resample); // "RS" by analogy
    CHECK(t1.insertFx[0].type == m::EffectType::Ducker);
    CHECK(t1.insertFx[0].type1 == 2); // SOURCE TRACK 3
    CHECK(t1.insertFx[0].sync);
    CHECK(t1.insertFx[1].type == m::EffectType::Flanger); // options of an earlier type: carried over, kept
    CHECK(t1.insertFx[1].type1 == 0);
    CHECK(t1.insertFx[1].extras.find("sp16.InsertFx.bType1")->value.find("FilterTypeLowPass") != std::string::npos);
    CHECK(has(report, "opaque-kept InsertFx.bType1 (an option of another effect type"));

    const auto& t2 = s0.tracks[2];
    CHECK(t2.insertFx[0].type == m::EffectType::ParametricEq2);
    CHECK(t2.insertFx[0].type1 == 3); // HI-SHELF
    CHECK(t2.insertFx[0].type2 == 2); // PEAK
    CHECK(t2.insertFx[1].type == m::EffectType::InsertDelay);
    CHECK(t2.routing.output == m::OutputAssign::Out56);
    CHECK(s0.tracks[5].routing.output == m::OutputAssign::Master); // AssignToNull

    const auto& t3 = s0.tracks[3];
    CHECK(t3.chokeGroup == 3);
    CHECK(t3.mode == m::TrackMode::Midi);
    CHECK(t3.midi.cc1[0] == m::MidiCcParam {20, 64, true});
    CHECK(t3.midi.pitchBendActive);
    CHECK(t3.midi.cc2[5].active);
    CHECK(t3.midi.cc2[0].cc == 71);
    CHECK(t3.midi.cc2Bypass);
    CHECK(t3.midi.program == 5);
    CHECK(t3.midi.length == 2.0);
    CHECK(t3.midi.notes[1] == 64);
    CHECK(s0.tracks[4].seq.retriggerSpeed == static_cast<int>(m::RetriggerSpeed::Whole)); // 1/1
    CHECK(s0.tracks[4].seq.stepShiftArea == 0); // a stored value is taken as is (BAR; the default is ALL)
    CHECK(s0.sendFx.type == m::EffectType::Reverb);
    CHECK(s0.masterFx.type == m::EffectType::Compressor);
    CHECK(p.ribbon.pitchTopCents == 1200);
    CHECK(p.ribbon.pitchBottomCents == -1200);
    CHECK(p.ribbon.user[0][0].target.empty()); // 255 = none

    // Steps: full velocity (value2 kept), the observed lock ordinals.
    const auto& pat = s0.patterns[0];
    for (const auto [step, track] : {std::pair {0, 0}, {4, 0}, {8, 0}, {12, 0}, {2, 1}, {10, 1}})
        CHECK(pat.steps[static_cast<std::size_t>(step)][static_cast<std::size_t>(track)].value2 == 127);
    auto lock = [&](int step, int track, std::string_view id) {
        const auto& c = pat.steps[static_cast<std::size_t>(step)][static_cast<std::size_t>(track)];
        REQUIRE(c.lockIndex != m::Step::kNoLocks);
        const auto& l = pat.locks[c.lockIndex];
        const int slot = id.substr(0, 2) == "fx" ? 28 : slotOf(id);
        REQUIRE((l.mask >> slot & 1u) != 0);
        return l.values[static_cast<std::size_t>(slot)];
    };
    CHECK(lock(4, 0, "playback.pitch") == -7.0f); // ordinal 0: cents → semitones
    CHECK(lock(4, 0, "track.volume") == 50.0f);   // ordinal 15
    CHECK(lock(8, 0, "track.pan") == -20.0f);     // ordinal 14
    CHECK(lock(8, 0, "env.release") == 30.0f);    // ordinal 7, k/127
    CHECK(lock(8, 0, "fx1.k5") == Approx(0.5).margin(1e-6));
    CHECK(lock(12, 0, "seq.retrigCount") == 1.0f); // ordinal 11
    CHECK(lock(12, 0, "seq.retrigSpeed") == 2.0f); // ordinal 12
    CHECK(has(report, "ordinal-unmapped ordinal 10"));
    // The model re-encodes every record exactly (FX empty words 5 + k need no per-cell data).
    auto tree = readContainer(interop_test::bytesOf(g.prj));
    REQUIRE(tree);
    const auto* steps = tree.value().children[0].children[16].prop("Steps");
    REQUIRE(steps != nullptr);
    auto records = decodeSteps(steps->bin);
    REQUIRE(records);
    CHECK(encodePattern(pat, ParamIndexMap::builtin()) == records.value());
    int cellsWithExtras = 0;
    for (const auto& e : pat.extras.entries)
        cellsWithExtras += e.key.rfind("sp16.step[", 0) == 0 ? 1 : 0;
    CHECK(cellsWithExtras == 2); // step 9's FX index byte, step 13's ordinal-10 word (value2: one blob per pattern)
    CHECK(pat.extras.find("sp16.PatternData.value2") != nullptr);

    // stepValue2 "velocity" reads value2 as the step velocity (0 → full).
    auto custom = ParamIndexMap::builtin();
    custom.stepValue2AsVelocity = true;
    const auto user = interop_test::temp().dir().getChildFile("velocity-map.json");
    REQUIRE(user.replaceWithText(juce::String(custom.toJson())));
    setUserMapFile(user);
    services::store::ImportReport r2;
    auto v = importProjectFile(g.prj, {}, r2);
    setUserMapFile(juce::File());
    REQUIRE(v);
    CHECK(v.value()->scenes[0].patterns[0].steps[4][0].value2 == 22);
    CHECK(v.value()->scenes[0].patterns[0].steps[0][0].value2 == 127);
}
