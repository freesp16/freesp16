// Mapping of synthetic reference projects to the model and the import report (FMT-040, FMT-042; spec sp16-interop.md
// "Node mapping", "Step matrix"; OQ-01/02/03/04/09/10).
#include "core/commands/sequencer/SequencerCommands.h"
#include "core/model/ModelOps.h"
#include "core/model/enums/EnumStrings.h"
#include "core/params/ParamRegistry.h"
#include "services/interop/sp16/Sp16Import.h"
#include "services/interop/sp16/Sp16ProjectCodec.h"
#include "services/store/SampleRefs.h"
#include "tests/unit/interop/Fixtures.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdio>
#include <functional>

using namespace services::interop::sp16;
namespace m = core::model;
using Catch::Approx;

namespace
{
struct Imported
{
    std::unique_ptr<m::Project> project;
    services::store::ImportReport report;
    bool has(std::string_view text) const
    {
        for (const auto* list : {&report.warnings, &report.unmapped})
            for (const auto& l : *list)
                if (l.find(text) != std::string::npos)
                    return true;
        return false;
    }
};

Imported import(const interop_test::Generated& g, ImportOptions opts = {})
{
    Imported out;
    auto r = importProjectFile(g.prj, opts, out.report);
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r);
    out.project = std::move(r.value());
    return out;
}

const m::StepLocks* locksOf(const m::Pattern& p, int step, int track)
{
    const auto& c = p.steps[static_cast<std::size_t>(step)][static_cast<std::size_t>(track)];
    return c.lockIndex == m::Step::kNoLocks ? nullptr : &p.locks[c.lockIndex];
}
bool locked(const m::StepLocks* l, int slot) { return l != nullptr && (l->mask >> slot & 1u) != 0; }
int slotOf(std::string_view id)
{
    return core::commands::sequencer::lockSlotOf(*core::builtinParams().find(id));
}

// Every PatternData of the file: the model re-encodes to the very records it was read from (model-level step
// round trip, FMT-043 for the step matrix).
void stepRoundTrip(const interop_test::Generated& g, const m::Project& p,
                   const ParamIndexMap& map = ParamIndexMap::builtin())
{
    auto tree = readContainer(interop_test::bytesOf(g.prj));
    REQUIRE(tree);
    int checked = 0;
    for (const auto& scene : tree.value().children)
    {
        const auto [base, s] = splitSuffix(scene.type);
        if (base != "SceneData")
            continue;
        for (const auto& pn : scene.children)
        {
            const auto [pb, q] = splitSuffix(pn.type);
            if (pb != "PatternData")
                continue;
            const int index = *q >= 16 ? *q % 16 : *q;
            auto records = decodeSteps(pn.prop("Steps")->bin);
            REQUIRE(records);
            const auto& pat = p.scenes[static_cast<std::size_t>(*s)].patterns[static_cast<std::size_t>(index)];
            CHECK(pat.extras.find("sp16.PatternData.Steps") == nullptr);
            CHECK(encodePattern(pat, map) == records.value());
            ++checked;
        }
    }
    CHECK(checked > 0);
}
} // namespace

TEST_CASE("prj import: basic project — scene, samples, triggers and a lock", "[interop]")
{
    INTEROP_FIXTURE(g, "basic");
    auto im = import(g);
    const auto& p = *im.project;
    CHECK(p.settings.tempo == 120.0);
    const auto& s0 = p.scenes[0];
    CHECK(s0.name == "BASIC");
    REQUIRE(s0.bpm.has_value());
    CHECK(*s0.bpm == 120.0);
    REQUIRE(p.sampleRefs.entries.size() == 2);
    for (const auto& e : p.sampleRefs.entries)
    {
        CHECK_FALSE(e.missing);
        CHECK(e.id.rfind("sp16:$Ext0/%Smp/", 0) == 0); // in place: the URL is the pool id (no hashing on import)
        CHECK(juce::File(e.path).existsAsFile());       // referenced in place (codec open path)
    }
    CHECK(p.sampleRefs.find(s0.tracks[0].playback.sample) != nullptr);
    CHECK(p.sampleRefs.find(s0.tracks[0].playback.sample)->displayName == "kick.wav");
    CHECK(s0.tracks[0].playback.length == 5292);
    const auto& pat = s0.patterns[0];
    CHECK(pat.length == 16);
    for (int step : {0, 4, 8, 12})
    {
        const auto& c = pat.steps[static_cast<std::size_t>(step)][0];
        CHECK(c.trigger == 100);
        CHECK(c.value2 == 127); // value2 is not read as a velocity (stepValue2 "ignore"): full, the stored one kept
        CHECK(c.triggerType == m::TriggerType::Full);

    }
    CHECK(pat.steps[2][1].trigger == 100);
    CHECK(pat.steps[1][0].trigger == m::Step::kEmpty);
    CHECK(pat.extras.find("sp16.PatternData.value2") != nullptr); // the stored value2 of the triggers, one blob
    const auto* l = locksOf(pat, 8, 0);
    REQUIRE(locked(l, slotOf("track.volume")));
    CHECK(l->values[static_cast<std::size_t>(slotOf("track.volume"))] == 40.0f);
    CHECK(pat.locks.size() == 1);
    CHECK(im.has("content scenes=16 tracks=256 patterns=64"));
    CHECK(im.has("samples referenced=2 found=2 missing=0"));
    CHECK(im.has("roundtrip tree identical"));
    CHECK(im.has("wavdat=2"));
    stepRoundTrip(g, p);
}

TEST_CASE("prj import: full project — settings, tracks, FX, unknown strings kept and reported", "[interop]")
{
    INTEROP_FIXTURE(g, "full");
    auto im = import(g);
    const auto& p = *im.project;
    const auto& st = p.settings;
    CHECK(st.tempo == 131.0);
    CHECK(st.masterVolume == 90);
    CHECK(st.swing == 58);
    CHECK(st.currentScene == 2);
    CHECK(st.currentPattern == 5);
    CHECK(st.afsEnabled);
    CHECK(st.afsRouteMode == m::AfsRouteMode::Post);
    CHECK(st.afsSendMode == static_cast<int>(m::AfsSendMode::PreFx));
    CHECK(st.softLimiter);
    CHECK(st.patternQuantize == m::QuantizeUnit::Bar);
    CHECK(st.padQuantize == m::QuantizeUnit::Step);
    CHECK(st.monitorVolume == 127); // 300 clamped, the original kept
    REQUIRE(st.extras.find("sp16.ProjectData.monitorVolume") != nullptr);
    CHECK(st.extras.find("sp16.ProjectData.monitorVolume")->value == "{\"int\":300}");
    CHECK(st.formatVersion == 1);
    CHECK(st.formatMinorVersion == 60);
    REQUIRE(st.extras.find("sp16.ProjectData.projectFilePath") != nullptr);
    CHECK(im.has("model-lacks ProjectData.projectFilePath"));

    const auto& s0 = p.scenes[0];
    CHECK(s0.name == "INTRO");
    CHECK_FALSE(s0.bpm.has_value()); // 0 = follow the project tempo
    CHECK(s0.colour == 4);
    CHECK(p.scenes[1].name == "VERSE");
    CHECK(*p.scenes[1].bpm == 98.5);
    CHECK(p.scenes[2].routing.assignMode == m::AssignMode::EightChannel);
    CHECK(p.scenes[2].routing.fxMode == m::FxMode::Send);
    CHECK(s0.sendFx.type == m::EffectType::Reverb);
    CHECK_FALSE(s0.sendFx.bypass); // FX-046: a stored Send/Master bypass loads off (the reference never reads it) …
    REQUIRE(s0.sendFx.extras.find("sp16.SendFx.bBypass") != nullptr); // … and is kept for the exporter
    CHECK_FALSE(im.has("SendFx.bypass"));
    CHECK(p.scenes[3].sendFx.type == m::EffectType::SendDelay);

    const auto& t0 = s0.tracks[0];
    CHECK(t0.volume == 110);
    CHECK(t0.pan == -20);
    CHECK(t0.chokeGroup == 2);
    CHECK(t0.mute);
    CHECK(t0.playback.start == 10);
    CHECK(t0.playback.length == 5000);
    CHECK(t0.playback.loop);
    CHECK(t0.playback.pitchCents == -300);
    CHECK(t0.playback.stretchMode == m::StretchMode::MasterTempo);
    CHECK(t0.ampEnv.attack == 0.25);
    CHECK(t0.ampEnv.release == 0.5);
    CHECK(t0.ampEnv.attackCurve == m::EnvelopeCurve::Curve);
    CHECK(t0.insertFx[0].type == m::EffectType::ParametricEq2);
    CHECK(t0.insertFx[0].params == std::array<double, 6> {0.5, 0.25, 1.0, 0.0, 0.75, 0.125});
    CHECK(t0.insertFx[1].type == m::EffectType::Off);
    REQUIRE(t0.insertFx[1].extras.find("sp16.InsertFx.effectType") != nullptr);
    CHECK(t0.insertFx[1].extras.find("sp16.InsertFx.effectType")->value == "SuperFx");
    CHECK(t0.send.channel == 1);
    CHECK(t0.send.amount == 64);
    CHECK(t0.lfo.depth == 30);
    CHECK(t0.lfo.extras.find("sp16.LfoData.type") != nullptr); // Pulse25: not in the model's LfoShape
    CHECK(t0.slices.extras.find("sp16.SliceData.grids") != nullptr); // odd length: not pairs, kept only
    CHECK(t0.slices.grids.empty());
    CHECK(s0.tracks[1].slices.grids == std::vector<std::int64_t> {3, 5000, 64, 7000}); // pair-shaped: read [G]
    CHECK(s0.tracks[1].slices.gridCount == 32);
    CHECK(s0.tracks[1].slices.extras.find("sp16.SliceData.grids") != nullptr);       // … and kept
    CHECK(im.has("guessed SliceData.grids read as (tag, frame) pairs"));

    const auto& t1 = s0.tracks[1];
    CHECK(t1.mode == m::TrackMode::Sample);
    CHECK(t1.extras.find("sp16.TrackData.trackMode")->value == "Granular");
    CHECK(t1.extras.find("sp16.TrackData.instrument")->value == "Juno");
    const auto* missing = p.sampleRefs.find(t1.playback.sample);
    REQUIRE(missing != nullptr);
    CHECK(missing->missing);
    CHECK(missing->id == "sp16:$Int0/%Tmp/missing.wav");
    const auto pathOf = [&](int t) { return p.sampleRefs.find(s0.tracks[static_cast<std::size_t>(t)].playback.sample)->path; };
    CHECK(s0.tracks[2].playback.sample == t0.playback.sample);        // the same URL: one pool entry
    CHECK(pathOf(6) == pathOf(0));                                    // found by name (the same file)
    CHECK(p.sampleRefs.find(s0.tracks[7].playback.sample)->missing);  // "../.." refused
    CHECK(pathOf(8) == pathOf(0));                                    // unknown alias: Samples/ tried
    CHECK(s0.tracks[3].mode == m::TrackMode::Midi);
    CHECK(s0.tracks[3].midi.notes[0] == 48);
    CHECK(s0.tracks[3].midi.velocity == 90);
    CHECK(s0.tracks[5].extras.find("sp16.TrackData.futureField") != nullptr);
    CHECK(p.extras.find("sp16.node.MysteryData") != nullptr);

    // Report (FMT-042): every class of issue the fixture holds.
    for (const char* line : {"enum-unmapped InsertFx.effectType='SuperFx'", "enum-unmapped LfoData.type='Pulse25'",
                             "enum-unmapped TrackData.trackMode='Granular'", "sample-missing $Int0/%Tmp/missing.wav",
                             "enum-unmapped step type code 9 (OQ-09)", "opaque-kept SliceData.grids",
                             "opaque-kept ArrangementData.Sections: arrangement not imported (OQ-02)",
                             "opaque-kept lock vct bits", "opaque-kept FX lock index byte", "unknown-node MysteryData",
                             "unknown-prop TrackData.futureField", "clamped ProjectData.monitorVolume 300",
                             "unsafe sample path refused", "sample found by file name only",
                             "path alias %Other unknown", "unknown-prop MidiCc1Data.bPitchBend",
                             "samples referenced=5 found=3 missing=2 byName=1"})
    {
        INFO(line);
        CHECK(im.has(line));
    }
    // Arrangement (OQ-02): not imported, blob kept.
    CHECK(p.arrangement.sections.empty());
    CHECK(p.arrangement.extras.find("sp16.ArrangementData.Sections") != nullptr);
    CHECK(p.arrangement.extras.find("sp16.ArrangementData.length")->value == "{\"int\":3}");
}

TEST_CASE("prj import: step types, every lock value class, FX locks, vct bits", "[interop]")
{
    INTEROP_FIXTURE(g, "full");
    auto im = import(g);
    const auto& pat = im.project->scenes[0].patterns[0];
    CHECK(pat.length == 32);
    CHECK(pat.trackMutes[2]);
    CHECK(pat.trackSolos[0]);
    CHECK_FALSE(pat.trackMutes[0]);
    CHECK(pat.steps[0][0].triggerType == m::TriggerType::Full);
    CHECK(pat.steps[1][0].triggerType == m::TriggerType::Half);
    CHECK(pat.steps[1][0].value2 == 127); // value2 not read as a velocity: full, the stored one kept
    const auto& lockOnly = pat.steps[2][0];
    CHECK(lockOnly.triggerType == m::TriggerType::LockOnly);
    CHECK(lockOnly.trigger == m::Step::kEmpty);
    // Raw class with our own scale: −1200 cents → −12 semitones.
    REQUIRE(locked(locksOf(pat, 2, 0), slotOf("playback.pitch")));
    CHECK(locksOf(pat, 2, 0)->values[static_cast<std::size_t>(slotOf("playback.pitch"))] == -12.0f);
    // Unknown code 9: Full in the model, the code kept for the round trip.
    CHECK(pat.extras.find("sp16.step[3].track[0]")->value == "{\"type\":9}");

    const auto* l = locksOf(pat, 4, 0);
    REQUIRE(l != nullptr);
    CHECK(l->values[static_cast<std::size_t>(slotOf("track.volume"))] == 64.0f);       // raw (ordinal 15)
    CHECK(l->values[static_cast<std::size_t>(slotOf("playback.start"))] == 1500000.0f); // frames, high part in 16
    CHECK_FALSE(locked(l, slotOf("seq.retrigCount")));                                 // slot 16 consumed
    CHECK(l->values[static_cast<std::size_t>(slotOf("env.attack"))] == 96.0f);          // k/127 (ordinal 5)
    CHECK(l->values[static_cast<std::size_t>(slotOf("playback.loop"))] == 1.0f);        // 127/127 → on
    CHECK(l->values[23] == Approx(0.5).margin(1e-6));                                   // FX1 k0
    CHECK(l->values[29] == Approx(0.25).margin(1e-6));                                  // FX2 k0
    for (int s = 0; s < m::kLockSlots; ++s)
        CHECK(l->change[static_cast<std::size_t>(s)] == m::LockChange::Revert);
    // Words the model cannot reproduce are kept per cell (FX2's index byte 40).
    const auto* raw = pat.extras.find("sp16.step[4].track[0]");
    REQUIRE(raw != nullptr);
    CHECK(raw->value.find("\"words\"") != std::string::npos);

    const auto* l2 = locksOf(pat, 5, 1);
    REQUIRE(l2 != nullptr);
    CHECK(l2->values[static_cast<std::size_t>(slotOf("playback.loop"))] == 1.0f); // index form 5 clamped to on
    CHECK(l2->values[static_cast<std::size_t>(slotOf("track.sendAmount"))] == 70.0f); // ordinal 13
    CHECK(pat.extras.find("sp16.step[5].track[1]")->value.find("\"vct\":{\"13\":2}") != std::string::npos);

    const auto* l3 = locksOf(pat, 6, 2);
    REQUIRE(l3 != nullptr);
    CHECK(l3->values[static_cast<std::size_t>(slotOf("playback.loopStart"))] == 44100.0f);
    CHECK(l3->values[static_cast<std::size_t>(slotOf("seq.offset"))] == 3.0f); // slot 16 standalone
    CHECK(slotOf("slice.number") == 22);
    CHECK(l3->values[22] == 5.0f); // ordinal 22 → slice.number

    stepRoundTrip(g, *im.project);
}

TEST_CASE("prj import: patterns by node suffix with gaps; absent scenes and patterns stay default (OQ-03)",
          "[interop]")
{
    {
        INTEROP_FIXTURE(g, "full");
        auto im = import(g);
        const auto& s1 = im.project->scenes[1];
        CHECK(s1.patterns[2].steps[0][4].trigger == 100);
        CHECK(s1.patterns[6].length == 8);
        CHECK(s1.patterns[15].length == 64);
        CHECK(s1.patterns[15].steps[63][15].value2 == 127);
        const auto def = m::defaultPattern();
        CHECK(s1.patterns[1] == def); // dropped from the file
        CHECK(s1.patterns[3] == def);
        CHECK(im.has("2.1,2.3,2.7,2.16"));
    }
    {
        INTEROP_FIXTURE(g, "sparse");
        auto im = import(g);
        const auto& p = *im.project;
        CHECK(p.scenes[0].patterns[1].steps[1][0].trigger == 100); // PatternData1 (scene × 16 + pattern)
        CHECK(p.scenes[4].patterns[0].length == 16);               // PatternData64
        CHECK(p.scenes[4].patterns[11].length == 12);              // PatternData75
        CHECK(p.scenes[0].patterns[0] == m::defaultPattern());
        CHECK(im.has("patterns=3 [1.2,5.1,5.12]"));
        CHECK(im.has("guessed PatternData suffix read as scene*16+pattern"));
        CHECK(im.has("structure SceneData absent (defaults) x2 (first: scene 15)"));
        CHECK(im.has("versionNo=1 minorVersionNo=50"));
        auto def = m::newDefaultProject();
        CHECK(p.scenes[14] == def->scenes[14]);
        CHECK(p.scenes[15] == def->scenes[15]);
        stepRoundTrip(g, p);
    }
}

TEST_CASE("prj import: edits after the import win over the kept raw words (exporter path)", "[interop]")
{
    INTEROP_FIXTURE(g, "full");
    auto im = import(g);
    auto pat = im.project->scenes[0].patterns[0];
    CHECK(encodePattern(pat, ParamIndexMap::builtin())[4 * 16].slots[29].idx == 40); // untouched: the kept byte
    auto& l = pat.locks[pat.steps[4][0].lockIndex];
    l.values[29] = 0.75f; // the user changed the FX2 k0 lock
    const auto recs = encodePattern(pat, ParamIndexMap::builtin());
    const auto& w = recs[4 * 16 + 0].slots[29];
    CHECK_FALSE(w.empty);
    CHECK(w.value22 == 1572863); // round(0.75 × 2097151), from the model, not the kept word
    CHECK(w.idx == 29);
    // A velocity edit wins over the kept value2 too.
    auto pat2 = im.project->scenes[0].patterns[0];
    pat2.steps[0][0].value2 = 64;
    CHECK(encodePattern(pat2, ParamIndexMap::builtin())[0].value2 == 64);
}

TEST_CASE("prj map: built-in param-index map, its shipped JSON copy and a user override", "[interop]")
{
    const auto& b = ParamIndexMap::builtin();
    CHECK(b.guesses() == 23);
    for (const auto& o : b.ordinals)
    {
        INFO(o.param);
        if (o.param.empty())
        {
            CHECK(o.ordinal == 10); // a small constant whose meaning is unknown: not imported
            continue;
        }
        const auto* spec = core::builtinParams().find(o.param);
        REQUIRE(spec != nullptr);
        CHECK(spec->lockable);
        CHECK(core::commands::sequencer::lockSlotOf(*spec) >= 0);
        for (const auto& other : b.ordinals) // no two ordinals on one parameter
            CHECK((other.ordinal == o.ordinal || other.param != o.param));
    }
    auto parsed = ParamIndexMap::parse(b.toJson());
    REQUIRE(parsed);
    CHECK(parsed.value().toJson() == b.toJson());
    const auto shipped = interop_test::sourceDir().getChildFile("src/services/interop/sp16/param-index-map.json");
    // Regenerate after a table change: <build>/tests/<name>_tests_interop "[.print-map]" > that file.
    CHECK(shipped.loadFileAsString().toStdString() == b.toJson());
    CHECK_FALSE(ParamIndexMap::parse("{\"ordinals\": [{\"ordinal\": 30}]}"));
    CHECK_FALSE(ParamIndexMap::parse("{\"ordinals\": [{\"ordinal\": 1, \"class\": \"frames\"}]}"));
    CHECK_FALSE(ParamIndexMap::parse("not json"));

    INTEROP_FIXTURE(g, "basic");
    const auto user = interop_test::temp().dir().getChildFile("user-map.json");
    auto custom = b;
    custom.ordinals[15].param = ""; // ordinal 15 (the volume lock) not imported
    custom.ordinals[15].guess = false;
    REQUIRE(user.replaceWithText(juce::String(custom.toJson())));
    setUserMapFile(user);
    auto im = import(g);
    setUserMapFile(juce::File());
    CHECK(im.has("param-index map: " + user.getFullPathName().toStdString() + " (22 of 23 flagged guess)"));
    CHECK(im.has("ordinal-unmapped ordinal 15"));
    const auto& pat = im.project->scenes[0].patterns[0];
    CHECK(pat.locks.empty());
    CHECK(pat.extras.find("sp16.step[8].track[0]") != nullptr); // the raw word is kept
    stepRoundTrip(g, *im.project, custom);                      // … so the bytes still round-trip

    REQUIRE(user.replaceWithText("{broken"));
    setUserMapFile(user);
    auto im2 = import(g);
    setUserMapFile(juce::File());
    CHECK(im2.has("ignored"));
    CHECK(im2.project->scenes[0].patterns[0].locks.size() == 1);
}

TEST_CASE("prj map: print the built-in table", "[.print-map]")
{
    std::fputs(ParamIndexMap::builtin().toJson().c_str(), stdout);
}

// LfoData.destination is the reference's modulation id (100–155 FX1, 200–255 FX2; research effects/parameters.md
// "FxParamIndex", F135): the LFO screen and the engine read the ParamSpec id in "native.destinationId", so an import
// that stored only the int lost every destination. The int stays for the export.
TEST_CASE("prj import: LFO destinations map to FX1 / FX2 parameter ids; others are reported", "[interop]")
{
    INTEROP_FIXTURE(g, "lfo");
    auto im = import(g);
    const auto& tracks = im.project->scenes[0].tracks;
    const auto dest = [&](int t) -> std::string
    {
        const auto* e = tracks[static_cast<std::size_t>(t)].lfo.extras.find("native.destinationId");
        return e == nullptr ? std::string() : e->value;
    };
    CHECK(dest(0) == "\"fx1.chorus.rate\"");
    CHECK(dest(1) == "\"fx2.eq2.b1freq\""); // id 212 = FX2's 112: EQ band 1 frequency (k 0)
    CHECK(dest(4) == "\"fx1.ducker.depth\"");
    for (const int t : {0, 1, 4})
    {
        const auto id = dest(t).substr(1, dest(t).size() - 2);
        const auto* spec = core::builtinParams().find(id);
        REQUIRE(spec != nullptr);
        CHECK(spec->lfoTarget);
    }
    CHECK(dest(2).empty()); // 154: the table's gap
    CHECK(dest(3).empty()); // 0: none, not reported
    CHECK(dest(5).empty());
    CHECK(tracks[1].lfo.destination == 212); // kept for the export
    CHECK(tracks[2].lfo.destination == 154);
    CHECK(im.has("enum-unmapped LfoData.destination=154"));
    CHECK(im.has("enum-unmapped LfoData.destination=7"));
    CHECK_FALSE(im.has("LfoData.destination=0"));
}
