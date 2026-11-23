// Stock → Sixteen and Sixteen → Sixteen compatibility of SP-16 imports (StepMatrix.h, MigrationV3.cpp):
//   * a reference project imports every step on its own track and step, with its locks, envelope times and mutes;
//   * a native bundle written by importer 1 (steps read step-major, envelope k/127, schema 2) migrates to exactly what
//     a fresh import gives — model cells, locks and the records the exporter writes;
//   * edits made in the app after such an import survive the migration (the app's cell wins a conflict, reported);
//   * the migration leaves projects made in the app alone, runs once, and chains from v1; a newer file is read-only.
// tests/fixtures/native/v2/sp16-legacy*.sixteen were written by importer 1 from synthetic/legacy.json (pinned save
// options, sample paths normalised; tests/fixtures/native/README.md). No device data.
#include "core/commands/sequencer/SequencerCommands.h"
#include "core/model/ModelOps.h"
#include "core/params/ParamRegistry.h"
#include "core/util/ampenv/EnvTime.h"
#include "services/interop/sp16/Sp16Import.h"
#include "services/interop/sp16/Sp16ProjectCodec.h"
#include "services/store/CodecRegistry.h"
#include "services/store/Migrations.h"
#include "services/store/NativeCodec.h"
#include "tests/unit/interop/Fixtures.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <memory>
#include <string>

using namespace services::interop::sp16;
namespace m = core::model;
namespace store = services::store;

namespace
{
juce::File nativeFixture(const char* version, const char* name)
{
    return interop_test::sourceDir().getChildFile("tests/fixtures/native").getChildFile(version).getChildFile(name);
}

struct Opened
{
    std::unique_ptr<m::Project> project = std::make_unique<m::Project>();
    store::ImportReport report;
    bool warned(std::string_view text) const
    {
        return std::any_of(report.warnings.begin(), report.warnings.end(),
                           [&](const std::string& w) { return w.find(text) != std::string::npos; });
    }
};

Opened openNative(const juce::File& bundle)
{
    Opened o;
    const auto r = store::openProjectInto(bundle, *o.project, o.report);
    INFO(bundle.getFullPathName() << ": " << (r.ok() ? std::string() : r.error().message));
    REQUIRE(r.ok());
    return o;
}

Opened importStock(const interop_test::Generated& g)
{
    Opened o;
    auto r = importProjectFile(g.prj, {}, o.report);
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r);
    o.project = std::move(r.value());
    return o;
}

const m::Step& cell(const m::Project& p, int scene, int pattern, int step1, int track1)
{
    return p.scenes[static_cast<std::size_t>(scene - 1)]
        .patterns[static_cast<std::size_t>(pattern - 1)]
        .steps[static_cast<std::size_t>(step1 - 1)][static_cast<std::size_t>(track1 - 1)];
}

bool on(const m::Project& p, int scene, int pattern, int step1, int track1)
{
    return cell(p, scene, pattern, step1, track1).trigger != m::Step::kEmpty;
}

float lockValue(const m::Project& p, int scene, int pattern, int step1, int track1, std::string_view id)
{
    const auto& pat = p.scenes[static_cast<std::size_t>(scene - 1)].patterns[static_cast<std::size_t>(pattern - 1)];
    const auto& c = cell(p, scene, pattern, step1, track1);
    REQUIRE(c.lockIndex != m::Step::kNoLocks);
    const auto* spec = core::builtinParams().find(id);
    REQUIRE(spec != nullptr);
    const int slot = core::commands::sequencer::lockSlotOf(*spec);
    const auto& l = pat.locks[c.lockIndex];
    INFO(id);
    REQUIRE((l.mask >> slot & 1u) != 0);
    return l.values[static_cast<std::size_t>(slot)];
}

int programmedCells(const m::Pattern& pat)
{
    int n = 0;
    for (const auto& row : pat.steps)
        for (const auto& c : row)
            n += c.trigger != m::Step::kEmpty ? 1 : 0;
    return n;
}

// The pattern with its extras sorted by key (the migration and the importer list cell extras in different orders).
m::Pattern sortedExtras(m::Pattern p)
{
    std::sort(p.extras.entries.begin(), p.extras.entries.end(),
              [](const auto& a, const auto& b) { return a.key < b.key; });
    return p;
}

juce::var parse(const char* json)
{
    auto v = juce::JSON::parse(juce::String::fromUTF8(json));
    REQUIRE(v.isObject());
    return v;
}

// A v2 migration root: one scene, one pattern file, as the native loader builds it.
juce::var rootV2(const juce::String& patternJson, const juce::String& projectExtras = "[]",
                 const juce::String& ampEnv = "{}")
{
    const auto text = R"({ "meta": { "format": { "name": "sixteen-project", "version": 2 } },
        "project": { "format": { "name": "sixteen-project", "version": 2 }, "_extras": )" +
                      projectExtras + R"(, "scenes": [ { "tracks": [ { "ampEnv": )" + ampEnv + R"( } ] } ] },
        "patterns": [ { "scene": 1, "patterns": [ )" +
                      patternJson + " ] } ] }";
    auto v = juce::JSON::parse(text);
    REQUIRE(v.isObject());
    return v;
}

// 1024 little-endian int32 as base64: the importer-1 blob "sp16.PatternData.value2", value at old cell k = s × 16 + t.
juce::String value2Blob(std::initializer_list<std::pair<int, int>> values)
{
    juce::MemoryBlock mb(1024 * 4, true);
    auto* b = static_cast<std::uint8_t*>(mb.getData());
    for (const auto& [k, v] : values)
        for (int i = 0; i < 4; ++i)
            b[4 * k + i] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(v) >> (8 * i));
    return juce::Base64::toBase64(mb.getData(), mb.getSize());
}

std::vector<std::string> migrate(juce::var& root, int from = 2)
{
    std::vector<std::string> applied;
    const auto r = store::MigrationRunner::builtin().run(root, from, &applied);
    REQUIRE(r.ok());
    CHECK(r.value() == store::kSchemaVersion);
    return applied;
}

const juce::var& firstPattern(const juce::var& root) { return root["patterns"][0]["patterns"][0]; }
} // namespace

// ------------------------------------------------------------------------------------------------ stock → sixteen
TEST_CASE("sp16 import: every step lands on its own track and step (track-major matrix)", "[interop][migration]")
{
    INTEROP_FIXTURE(g, "legacy");
    const auto im = importStock(g);
    const auto& p = *im.project;

    // Scene 1 pattern 1: the four-on-the-floor kick is on track 1, not spread over tracks 1 / 5 / 9 / 13.
    for (int s : {1, 5, 9, 13})
    {
        INFO("kick step " << s);
        CHECK(on(p, 1, 1, s, 1));
        CHECK(cell(p, 1, 1, s, 1).trigger == 15);
        CHECK(cell(p, 1, 1, s, 1).value2 == 127);
    }
    for (int t : {5, 9, 13})
        CHECK_FALSE(on(p, 1, 1, 1, t)); // where the step-major reading put them
    for (int s = 1; s <= 15; s += 2)
        CHECK(on(p, 1, 1, s, 3)); // the hats: every odd step of track 3
    CHECK(on(p, 1, 1, 3, 6));
    const auto& p1 = p.scenes[0].patterns[0];
    CHECK(programmedCells(p1) == 17);
    CHECK(p1.length == 16);

    // Locks are the record's own: pitch / pan on track 3, envelope and mixer locks on track 4, FX locks on track 2.
    CHECK(lockValue(p, 1, 1, 3, 3, "playback.pitch") == -5.0f);
    CHECK(lockValue(p, 1, 1, 7, 3, "playback.pitch") == 12.0f);
    CHECK(lockValue(p, 1, 1, 7, 3, "track.pan") == -29.0f);
    CHECK(lockValue(p, 1, 1, 1, 4, "env.attack") == 70.0f);   // k/127 on the device = index k in the model
    CHECK(lockValue(p, 1, 1, 1, 4, "env.hold") == 100.0f);
    CHECK(lockValue(p, 1, 1, 1, 4, "env.release") == 92.0f);  // importer 1 made 93 of it (round(92 × 128 / 127))
    CHECK(lockValue(p, 1, 1, 9, 4, "env.release") == 18.0f);
    CHECK(lockValue(p, 1, 1, 9, 4, "track.volume") == 90.0f);
    CHECK(lockValue(p, 1, 1, 9, 4, "track.sendAmount") == 61.0f);
    {
        const auto& c = cell(p, 1, 1, 5, 2);
        REQUIRE(c.lockIndex != m::Step::kNoLocks);
        const auto& l = p1.locks[c.lockIndex];
        CHECK((l.mask >> 28 & 1u) != 0); // FX1 k5: Chorus index 4
        CHECK((l.mask >> 34 & 1u) != 0); // FX2 k5: InsertDelay index 16
        CHECK(l.values[34] == Catch::Approx(0.5).margin(1e-6));
    }
    // Every record named its own position, FX index bytes match the track's effects: nothing kept per cell.
    for (const auto& e : p1.extras.entries)
        CHECK(e.key.rfind("sp16.step[", 0) != 0);
    CHECK_FALSE(im.warned("names another track / step"));

    // Pattern 2 (32 steps): track 16, a retrigger lock on track 4 step 21.
    CHECK(p.scenes[0].patterns[1].length == 32);
    CHECK(on(p, 1, 2, 32, 16));
    CHECK(on(p, 1, 2, 2, 16));
    CHECK(on(p, 1, 2, 17, 1));
    CHECK(lockValue(p, 1, 2, 21, 4, "seq.retrigCount") == 1.0f);
    CHECK(lockValue(p, 1, 2, 21, 4, "seq.retrigSpeed") == 2.0f);
    // Pattern 3 (64 steps): the corners of the matrix.
    CHECK(on(p, 1, 3, 64, 16));
    CHECK(on(p, 1, 3, 64, 1));
    CHECK(on(p, 1, 3, 1, 2));
    CHECK(on(p, 1, 3, 40, 9));
    CHECK(programmedCells(p.scenes[0].patterns[2]) == 4);
    // Scene 2.
    CHECK(on(p, 2, 1, 1, 1));
    CHECK(on(p, 2, 1, 4, 5));
    CHECK(lockValue(p, 2, 1, 16, 12, "playback.pitch") == -12.0f);

    // Mutes are per scene and track (TrackData.muteSetting).
    CHECK(p.scenes[0].tracks[1].mute);
    CHECK(p.scenes[0].tracks[5].mute);
    CHECK_FALSE(p.scenes[0].tracks[0].mute);
    CHECK(p.scenes[1].tracks[4].mute);
    CHECK_FALSE(p.scenes[1].tracks[1].mute);

    // Envelope times: the stored k/127 is the model's index k (k/128); 1.0 hold / release is INFINITY.
    const auto& t1 = p.scenes[0].tracks[0].ampEnv;
    CHECK(t1.attack == core::ampenv::storedFromIndex(23));
    CHECK(t1.release == core::ampenv::storedFromIndex(92));
    CHECK(p.scenes[0].tracks[1].ampEnv.hold == core::ampenv::storedFromIndex(18));
    CHECK(p.scenes[0].tracks[2].ampEnv.hold == 0.0f);
    CHECK(p.scenes[0].tracks[2].ampEnv.release == core::ampenv::storedFromIndex(2));

    // Provenance.
    CHECK(p.origin.format == "sp16-prj");
    CHECK(p.origin.importer == kImporterVersion);
    CHECK(p.origin.sourceVersion == 3);
    CHECK(p.origin.sourceMinorVersion == 1);
    CHECK(p.origin.file == g.prj.getFileName().toStdString());
    CHECK_FALSE(p.origin.contentId.empty());
}

TEST_CASE("sp16 import: the records it writes back name their own track and step", "[interop][migration]")
{
    INTEROP_FIXTURE(g, "legacy");
    const auto im = importStock(g);
    auto tree = readContainer(interop_test::bytesOf(g.prj));
    REQUIRE(tree);
    int patterns = 0;
    for (const auto& scene : tree.value().children)
    {
        const auto [base, s] = splitSuffix(scene.type);
        if (base != "SceneData")
            continue;
        const auto& sc = im.project->scenes[static_cast<std::size_t>(*s)];
        const auto fx = insertTypesOf(sc);
        for (const auto& pn : scene.children)
        {
            const auto [pb, q] = splitSuffix(pn.type);
            if (pb != "PatternData")
                continue;
            const auto stored = decodeSteps(pn.prop("Steps")->bin);
            REQUIRE(stored);
            const auto written = encodePattern(sc.patterns[static_cast<std::size_t>(*q % 16)], ParamIndexMap::builtin(), &fx);
            CHECK(written == stored.value());
            for (std::size_t k = 0; k < written.size(); ++k)
                if (written[k].programmed())
                {
                    CHECK(written[k].trigger == trackOfRecord(k));
                    CHECK(written[k].value2 == stepOfRecord(k));
                }
            if (std::any_of(written.begin(), written.end(), [](const StepRecord& r) { return r.programmed(); }))
                ++patterns;
        }
    }
    CHECK(patterns == 4); // scene 1 patterns 1–3, scene 2 pattern 1
}

// ------------------------------------------------------------------------------------------------ sixteen v2 → v3
TEST_CASE("migration v2 → v3: an importer-1 bundle becomes what a fresh import gives", "[interop][migration]")
{
    INTEROP_FIXTURE(g, "legacy");
    const auto fresh = importStock(g);
    const auto old = openNative(nativeFixture("v2", "sp16-legacy.sixteen"));
    const auto& a = *fresh.project;
    const auto& b = *old.project;

    for (std::size_t s = 0; s < a.scenes.size(); ++s)
    {
        INFO("scene " << s + 1);
        const auto fx = insertTypesOf(a.scenes[s]);
        for (std::size_t t = 0; t < 16; ++t)
        {
            INFO("track " << t + 1);
            CHECK(a.scenes[s].tracks[t].ampEnv == b.scenes[s].tracks[t].ampEnv);
            CHECK(a.scenes[s].tracks[t].mute == b.scenes[s].tracks[t].mute);
            CHECK(a.scenes[s].tracks[t].insertFx == b.scenes[s].tracks[t].insertFx);
        }
        for (std::size_t q = 0; q < 16; ++q)
        {
            INFO("pattern " << q + 1);
            const auto& pa = a.scenes[s].patterns[q];
            const auto& pb = b.scenes[s].patterns[q];
            CHECK(pa.length == pb.length);
            CHECK(pa.trackMutes == pb.trackMutes);
            CHECK(pa.steps == pb.steps);
            CHECK(pa.locks == pb.locks);
            CHECK(pb.extras.find("sp16.PatternData.value2") == nullptr);
            // The exporter writes the same records from both (kept raw words included).
            CHECK(encodePattern(pa, ParamIndexMap::builtin(), &fx) == encodePattern(pb, ParamIndexMap::builtin(), &fx));
        }
    }
    CHECK(b.origin.format == "sp16-prj");
    CHECK(b.origin.importer == 1); // the version that wrote it, recorded by the migration
    CHECK(old.warned("converted from an older format: project origin recorded"));
    CHECK(old.warned("SP-16 import: 29 step(s) in 4 pattern(s) moved to their track and step"));
    CHECK_FALSE(old.warned("dropped"));
    CHECK(old.warned("envelope time(s) rescaled"));
    CHECK_FALSE(old.report.readOnly);
}

TEST_CASE("migration v2 → v3: edits made after an importer-1 import survive", "[interop][migration]")
{
    // The edits (in importer 1's coordinates, scene 1 pattern 1): a step added at model (2, 8) — free in both layouts;
    // a step added at model (5, 1) with velocity 64 — where the kick of step 5 belongs; the scrambled kick of step 5
    // (model (1, 5)) set to velocity 90; the scrambled kick of step 9 (model (1, 9)) deleted.
    const auto o = openNative(nativeFixture("v2", "sp16-legacy-edited.sixteen"));
    const auto& p = *o.project;
    CHECK(on(p, 1, 1, 2, 8));
    CHECK(cell(p, 1, 1, 2, 8).value2 == 100);
    CHECK(on(p, 1, 1, 5, 1));
    CHECK(cell(p, 1, 1, 5, 1).value2 == 64); // the app's step wins; the imported kick is dropped
    CHECK(on(p, 1, 1, 1, 1));
    CHECK(on(p, 1, 1, 13, 1));
    CHECK_FALSE(on(p, 1, 1, 9, 1)); // deleted in the app
    for (int t : {5, 9, 13})
        CHECK_FALSE(on(p, 1, 1, 1, t));
    CHECK(o.warned("1 dropped where a step made in the app now sits"));
    // The rest is migrated as without edits.
    CHECK(lockValue(p, 1, 1, 1, 4, "env.release") == 92.0f);
    CHECK(on(p, 1, 3, 64, 16));
    std::vector<std::string> problems;
    CHECK(m::validate(p, problems));
}

TEST_CASE("migration v2 → v3: once migrated and saved, the bundle opens unchanged", "[interop][migration]")
{
    const auto first = openNative(nativeFixture("v2", "sp16-legacy.sixteen"));
    const auto dir = interop_test::temp().dir().getChildFile("migrated-legacy.sixteen");
    dir.deleteRecursively();
    REQUIRE(store::saveProject(*first.project, dir).ok());
    const auto again = openNative(dir);
    CHECK_FALSE(again.warned("converted from an older format"));
    CHECK(*again.project == *first.project);
    // and equals the pinned expected/v2 bundle the fixture test checks
    const auto expected = openNative(nativeFixture("expected/v2", "sp16-legacy.sixteen"));
    CHECK(*expected.project == *first.project);
}

TEST_CASE("migration v2 → v3: projects made in the app are left alone", "[interop][migration]")
{
    // beat.sixteen (v2, made in the app): trigger 0 cells, no import blob.
    const auto o = openNative(nativeFixture("v2", "beat.sixteen"));
    const auto& p = *o.project;
    for (int s : {1, 5, 9, 13})
        CHECK(cell(p, 1, 1, s, 1).trigger == 0);
    CHECK(on(p, 1, 1, 5, 2));
    CHECK_FALSE(p.origin.imported());
    CHECK_FALSE(o.warned("SP-16 import: "));

    // An app project whose envelope happens to sit on the k/127 grid is not rescaled (no import evidence).
    auto root = rootV2(R"({ "index": 1, "steps": [ { "track": 5, "step": 1, "trigger": 0, "velocity": 99 } ] })", "[]",
                       R"({ "attack": 0.07874015718698502 })");
    const auto notes = migrate(root);
    CHECK(notes.size() == 1);
    CHECK(static_cast<double>(root["project"]["scenes"][0]["tracks"][0]["ampEnv"]["attack"]) == 0.07874015718698502);
    CHECK(static_cast<int>(firstPattern(root)["steps"][0]["track"]) == 5);
    CHECK_FALSE(root["project"].hasProperty("origin"));
}

TEST_CASE("migration v2 → v3: unit cases on the migration root", "[interop][migration]")
{
    SECTION("an imported cell moves; an app cell at its destination wins; extras follow their cell")
    {
        // old (s=1, t=5) holds the record k = 4: track 1 step 5 (trigger 0, blob 4) → moves to (5, 1).
        // old (s=1, t=2) holds k = 1: track 1 step 2 → its destination (2, 1) holds an app cell (trigger 15): dropped.
        // old (s=3, t=1) is an app cell (trigger 11): stays.
        // A lone extras entry of old (1, 7) = k 6 (no step entry) moves to step 7 track 1 → sp16.step[6].track[0].
        const auto blob = value2Blob({{4, 4}, {1, 1}, {6, 6}});
        auto root = rootV2(R"({ "index": 1, "steps": [
            { "track": 5, "step": 1, "trigger": 0, "velocity": 127, "type": "Half" },
            { "track": 2, "step": 1, "trigger": 0, "velocity": 127 },
            { "track": 1, "step": 2, "trigger": 15, "velocity": 80 },
            { "track": 1, "step": 3, "trigger": 11, "velocity": 70, "type": "Half" } ],
            "_extras": [ { "key": "sp16.PatternData.value2", "encoding": "base64", "value": ")" + blob + R"(" },
                         { "key": "sp16.step[0].track[4]", "encoding": "json", "value": "{\"vct\":{\"3\":1}}" },
                         { "key": "sp16.step[0].track[1]", "encoding": "json", "value": "{\"vct\":{\"4\":1}}" },
                         { "key": "sp16.step[0].track[6]", "encoding": "json", "value": "{\"words\":{\"9\":5}}" } ] })");
        const auto notes = migrate(root);
        REQUIRE(notes.size() == 2);
        CHECK(notes[1].find("1 step(s) in 1 pattern(s) moved") != std::string::npos);
        CHECK(notes[1].find("1 dropped") != std::string::npos);
        const auto& pat = firstPattern(root);
        const auto* steps = pat["steps"].getArray();
        REQUIRE(steps != nullptr);
        REQUIRE(steps->size() == 3);
        // model order: step, then track
        CHECK(static_cast<int>((*steps)[0]["step"]) == 2);
        CHECK(static_cast<int>((*steps)[0]["trigger"]) == 15);
        CHECK(static_cast<int>((*steps)[0]["velocity"]) == 80);
        CHECK(static_cast<int>((*steps)[1]["step"]) == 3);
        CHECK(static_cast<int>((*steps)[1]["trigger"]) == 11);
        CHECK(static_cast<int>((*steps)[2]["step"]) == 5);
        CHECK(static_cast<int>((*steps)[2]["track"]) == 1);
        CHECK(static_cast<int>((*steps)[2]["trigger"]) == 11); // the code of its type (Half), as a fresh import
        std::map<juce::String, juce::String> extras;
        for (const auto& e : *pat["_extras"].getArray())
            extras[e["key"].toString()] = e["value"].toString();
        CHECK(extras.size() == 2);
        CHECK(extras.count("sp16.PatternData.value2") == 0);
        CHECK(extras["sp16.step[4].track[0]"] == "{\"vct\":{\"3\":1}}");
        CHECK(extras["sp16.step[6].track[0]"] == "{\"words\":{\"9\":5}}");
        CHECK(root["project"]["origin"]["format"].toString() == "sp16-prj");
        CHECK(static_cast<int>(root["project"]["origin"]["importer"]) == 1);
    }
    SECTION("HOLD / RELEASE locks of moved cells: round(k·128/127) back to k, 127 to INFINITY; edited ones stay")
    {
        constexpr int kK127 = 16513;
        const auto word = [&](int k, int ordinal) { return juce::String(static_cast<juce::int64>(k * kK127) << 8 | ordinal); };
        const auto blob = value2Blob({{16, 16}, {32, 32}});
        // k = 16: old (step 2, track 1) holds track 1 step 17; k = 32: old (step 3, track 1) holds track 1 step 33.
        auto root = rootV2(R"({ "index": 1, "steps": [
            { "track": 1, "step": 2, "trigger": 0, "velocity": 127, "locks": { "hold": 93, "release": 128 } },
            { "track": 1, "step": 3, "trigger": 0, "velocity": 127, "locks": { "release": 50 } } ],
            "_extras": [ { "key": "sp16.PatternData.value2", "encoding": "base64", "value": ")" + blob + R"(" },
                { "key": "sp16.step[1].track[0]", "encoding": "json",
                  "value": "{\"words\":{\"6\":)" + word(92, 6) + R"(,\"7\":)" + word(127, 7) + R"(}}" },
                { "key": "sp16.step[2].track[0]", "encoding": "json", "value": "{\"words\":{\"7\":)" + word(40, 7) +
                           R"(}}" } ] })");
        migrate(root);
        const auto* steps = firstPattern(root)["steps"].getArray();
        REQUIRE(steps->size() == 2);
        CHECK(static_cast<int>((*steps)[0]["track"]) == 1);
        CHECK(static_cast<int>((*steps)[0]["step"]) == 17);
        CHECK(static_cast<int>((*steps)[0]["locks"]["hold"]) == 92);     // 92 → importer 1: 93 → 92
        CHECK(static_cast<int>((*steps)[0]["locks"]["release"]) == 128); // 127 → importer 1: 128 (finite) → INFINITY
        CHECK(static_cast<int>((*steps)[1]["track"]) == 1);
        CHECK(static_cast<int>((*steps)[1]["step"]) == 33);
        CHECK(static_cast<int>((*steps)[1]["locks"]["release"]) == 50); // 40 imported as 40: edited since, kept
    }
    SECTION("envelope norms of an import: k/127 → k/128; 0, 1 and k/128 values stay")
    {
        auto root = rootV2(R"({ "index": 1 })", R"([ { "key": "sp16.ProjectData.x", "encoding": "text", "value": "1" } ])",
                           R"({ "attack": 0.07874015718698502, "hold": 1.0, "release": 0.5 })");
        const auto notes = migrate(root);
        const auto& env = root["project"]["scenes"][0]["tracks"][0]["ampEnv"];
        CHECK(static_cast<double>(env["attack"]) == static_cast<double>(core::ampenv::storedFromIndex(10)));
        CHECK(static_cast<double>(env["hold"]) == 1.0);
        CHECK(static_cast<double>(env["release"]) == 0.5); // 64/128: the app's own grid
        REQUIRE(notes.size() == 2);
        CHECK(notes[1].find("1 envelope time(s) rescaled") != std::string::npos);
        CHECK(root["project"]["origin"]["format"].toString() == "sp16-prj");
    }
    SECTION("a damaged blob: removed, the cells stay")
    {
        auto root = rootV2(R"({ "index": 1, "steps": [ { "track": 5, "step": 1, "trigger": 0, "velocity": 127 } ],
            "_extras": [ { "key": "sp16.PatternData.value2", "encoding": "base64", "value": "AAAA" } ] })");
        migrate(root);
        CHECK(static_cast<int>(firstPattern(root)["steps"][0]["track"]) == 5);
        CHECK_FALSE(firstPattern(root).hasProperty("_extras"));
    }
    SECTION("an existing origin is kept")
    {
        auto root = rootV2(R"({ "index": 1 })", R"([ { "key": "sp16.x", "encoding": "text", "value": "1" } ])");
        root["project"].getDynamicObject()->setProperty("origin", parse(R"({ "format": "other", "importer": 7 })"));
        migrate(root);
        CHECK(root["project"]["origin"]["format"].toString() == "other");
        CHECK(static_cast<int>(root["project"]["origin"]["importer"]) == 7);
    }
}

TEST_CASE("migration: v1 bundles chain through v2 to v3; a newer bundle opens read-only", "[interop][migration]")
{
    for (const char* name : {"default.sixteen", "beat.sixteen", "random-42.sixteen"})
    {
        INFO(name);
        const auto o = openNative(nativeFixture("v1", name));
        std::vector<std::string> converted;
        for (const auto& w : o.report.warnings)
            if (w.rfind("converted from an older format: ", 0) == 0)
                converted.push_back(w);
        REQUIRE(converted.size() >= 2);
        CHECK(converted.back().find("project origin recorded") != std::string::npos); // 2 → 3 last
        CHECK_FALSE(o.report.readOnly);
    }

    // A bundle of a future schema: read-only, never migrated down, never rewritten.
    const auto src = nativeFixture("v3", "beat.sixteen");
    const auto dir = interop_test::temp().dir().getChildFile("future.sixteen");
    dir.deleteRecursively();
    REQUIRE(src.copyDirectoryTo(dir));
    for (const char* f : {"meta.json", "project.json"})
    {
        auto file = dir.getChildFile(f);
        const auto text = file.loadFileAsString();
        const auto from = "\"version\": " + juce::String(store::kSchemaVersion) + " }";
        REQUIRE(text.contains(from));
        REQUIRE(file.replaceWithText(text.replace(from, "\"version\": " + juce::String(store::kSchemaVersion + 1) + " }")));
    }
    const auto o = openNative(dir);
    CHECK(o.report.readOnly);
    CHECK(o.warned("newer version"));
    CHECK(cell(*o.project, 1, 1, 1, 1).trigger == 0);
}
