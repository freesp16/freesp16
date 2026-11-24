// Schema versions and migrations (FMT-003): the chain runner, the built-in chain, and every fixture in
// tests/fixtures/native/v*/ migrating to the current version. Fixtures are written by this app's own writer
// (APP_REGENERATE_FIXTURES=1 rewrites them; never device data).
#include "tests/unit/store/StoreTestSupport.h"

#include "services/store/CodecRegistry.h"
#include "services/store/Migrations.h"

#include <catch2/catch_test_macros.hpp>

using namespace services::store;

namespace
{
// The 05 §5.6 worked example, on the migration root: v2 adds Track.fadeIn = 0.
void addFadeIn(juce::var& root)
{
    if (const auto* scenes = root["project"]["scenes"].getArray())
        for (const auto& scene : *scenes)
            if (const auto* tracks = scene["tracks"].getArray())
                for (const auto& track : *tracks)
                    if (auto* t = track.getDynamicObject(); t != nullptr && ! t->hasProperty("fadeIn"))
                        t->setProperty("fadeIn", 0);
}
void renameSwing(juce::var& root)
{
    auto* settings = root["project"]["settings"].getDynamicObject();
    if (settings != nullptr && settings->hasProperty("swing"))
    {
        settings->setProperty("swingPercent", settings->getProperty("swing"));
        settings->removeProperty("swing");
    }
}

juce::var sampleRoot()
{
    return juce::JSON::parse(R"({ "meta": { "format": { "name": "sixteen-project", "version": 1 } },
        "project": { "settings": { "swing": 60 }, "scenes": [ { "tracks": [ {}, { "fadeIn": 5 } ] } ] },
        "patterns": [] })");
}

juce::File fixtureRoot() { return juce::File(APP_SOURCE_DIR).getChildFile("tests/fixtures/native"); }

// The generators behind the fixtures (named by the fixture's bundle name).
std::unique_ptr<core::model::Project> generateFixture(const juce::String& name)
{
    if (name == "default")
        return core::model::newDefaultProject();
    if (name == "beat")
    {
        auto p = core::model::newDefaultProject();
        auto& pattern = p->scenes[0].patterns[0];
        for (int s = 0; s < 16; ++s)
        {
            auto& kick = pattern.steps[static_cast<std::size_t>(s)][0];
            if (s % 4 == 0)
                kick.trigger = 0, kick.value2 = 120;
            auto& hat = pattern.steps[static_cast<std::size_t>(s)][2];
            if (s % 2 == 0)
                hat.trigger = 0, hat.value2 = static_cast<std::int16_t>(s % 4 == 0 ? 100 : 70);
        }
        pattern.steps[4][1].trigger = 0;
        pattern.steps[12][1].trigger = 0;
        p->scenes[0].name = "Beat";
        p->settings.tempo = 92.5;
        return p;
    }
    return storetest::randomProject(42, 0.05);
}
} // namespace

TEST_CASE("migration: the runner walks the chain and stamps each version", "[store][migration]")
{
    const Migration chain[] = {{2, 3, &renameSwing, "settings.swing is swingPercent"},
                               {1, 2, &addFadeIn, "Track.fadeIn defaults to 0"},
                               {3, 4, &addFadeIn, "beyond the target: ignored"}};
    const MigrationRunner runner(chain, 3);
    CHECK(runner.validate().empty());

    auto root = sampleRoot();
    std::vector<std::string> applied;
    const auto reached = runner.run(root, 1, &applied);
    REQUIRE(reached.ok());
    CHECK(reached.value() == 3);
    CHECK(schemaVersionOf(root) == 3);
    REQUIRE(applied.size() == 2);
    CHECK(applied[0] == "Track.fadeIn defaults to 0");
    CHECK(static_cast<int>(root["project"]["scenes"][0]["tracks"][0]["fadeIn"]) == 0);
    CHECK(static_cast<int>(root["project"]["scenes"][0]["tracks"][1]["fadeIn"]) == 5); // existing values stay
    CHECK(static_cast<int>(root["project"]["settings"]["swingPercent"]) == 60);

    auto newer = sampleRoot();
    CHECK(runner.run(newer, 7).value() == 7); // newer than the target: untouched (the caller opens read-only)
    CHECK_FALSE(runner.run(newer, 0).ok());
}

TEST_CASE("migration: gaps and duplicates are reported", "[store][migration]")
{
    const Migration gap[] = {{1, 2, &addFadeIn, "a"}};
    const MigrationRunner withGap(gap, 3);
    CHECK_FALSE(withGap.validate().empty());
    auto root = sampleRoot();
    const auto r = withGap.run(root, 1);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().detail.find("version 2") != std::string::npos);

    const Migration twice[] = {{1, 2, &addFadeIn, "a"}, {1, 2, &renameSwing, "b"}};
    CHECK_FALSE(MigrationRunner(twice, 2).validate().empty());
}

TEST_CASE("migration: the built-in chain reaches the current schema", "[store][migration]")
{
    const auto& runner = MigrationRunner::builtin();
    CHECK(runner.currentVersion() == kSchemaVersion);
    const auto problems = runner.validate();
    INFO((problems.empty() ? std::string() : problems.front()));
    CHECK(problems.empty());
}

TEST_CASE("migration: schema 4 writes MIDI NOTE2-4 OFF as 0 (semitones from NOTE)", "[store][migration]")
{
    auto root = juce::JSON::parse(R"({ "meta": { "format": { "name": "sixteen-project", "version": 3 } },
        "project": { "scenes": [ { "tracks": [ { "midi": { "notes": [62, 4, -1, -1] } },
                                               { "mode": "Midi" } ] } ] } })");
    std::vector<std::string> applied;
    const auto reached = MigrationRunner::builtin().run(root, 3, &applied);
    REQUIRE(reached.ok());
    CHECK(reached.value() == 4);
    const auto& notes = root["project"]["scenes"][0]["tracks"][0]["midi"]["notes"];
    CHECK(static_cast<int>(notes[0]) == 62);
    CHECK(static_cast<int>(notes[1]) == 4); // an imported offset stays: +4
    CHECK(static_cast<int>(notes[2]) == 0); // OFF
    CHECK(static_cast<int>(notes[3]) == 0);
    CHECK_FALSE(root["project"]["scenes"][0]["tracks"][1].hasProperty("midi")); // absent stays absent (the default)
}

TEST_CASE("migration: schema 4 restores the SP-16 importer's NOTE2-4 offsets kept in the extras; idempotent",
          "[store][migration]")
{
    // Schema 3's importer read the reference's offsets as notes −1…127: 0 (OFF) and every negative offset became −1,
    // the original kept in the track's MIDI extras. Those are the offsets.
    const auto extra = [](int k, const char* value)
    {
        return juce::String(R"({ "key": "sp16.MidiNoteData.note)") + juce::String(k) +
               R"(", "encoding": "json", "value": ")" + juce::String(value).replace("\"", "\\\"") + "\" }";
    };
    const auto text = juce::String(R"({ "meta": { "format": { "name": "sixteen-project", "version": 3 } },
        "project": { "scenes": [ { "tracks": [
            { "midi": { "notes": [62, 4, -1, -1], "_extras": [)") +
                      extra(3, R"({"int":-5})") + ", " + extra(4, R"({"int":0})") + R"(] } },
            { "midi": { "program": 3, "_extras": [)" +
                      extra(2, R"({"int":-12})") + R"(] } },
            { "midi": { "notes": [60, -1, -1, -1], "_extras": [)" +
                      extra(2, R"({"int":300})") + ", " + extra(3, R"({"string":"x"})") + R"(] } } ] } ] } })";
    auto root = juce::JSON::parse(text);
    REQUIRE(root.isObject());
    REQUIRE(MigrationRunner::builtin().run(root, 3).ok());
    const auto notes = [&root](int t) { return root["project"]["scenes"][0]["tracks"][t]["midi"]["notes"]; };
    CHECK(static_cast<int>(notes(0)[0]) == 62);
    CHECK(static_cast<int>(notes(0)[1]) == 4);  // in range when imported: no extras, stays
    CHECK(static_cast<int>(notes(0)[2]) == -5); // −1 in the model, −5 in the extras: a fourth below
    CHECK(static_cast<int>(notes(0)[3]) == 0);  // the reference's 0: OFF
    // No "notes" (the schema 3 default) but an offset in the extras: the array is made.
    REQUIRE(notes(1).isArray());
    CHECK(static_cast<int>(notes(1)[0]) == 60);
    CHECK(static_cast<int>(notes(1)[1]) == -12);
    CHECK(static_cast<int>(notes(1)[2]) == 0);
    // An extras value that is not an offset (out of range, not an int): the current rule (−1 → OFF).
    CHECK(static_cast<int>(notes(2)[1]) == 0);
    CHECK(static_cast<int>(notes(2)[2]) == 0);

    // Idempotent: the same rewrite again changes nothing (a restored −1 keeps its extras entry).
    const auto once = juce::JSON::toString(root);
    REQUIRE(MigrationRunner::builtin().run(root, 3).ok());
    CHECK(juce::JSON::toString(root) == once);
    auto minusOne =
        juce::JSON::parse(juce::String(R"({ "meta": { "format": { "name": "sixteen-project", "version": 3 } },
        "project": { "scenes": [ { "tracks": [ { "midi": { "notes": [60, -1, 5, -1], "_extras": [)") +
                          extra(2, R"({"int":-1})") + R"(] } } ] } ] } })");
    REQUIRE(MigrationRunner::builtin().run(minusOne, 3).ok());
    REQUIRE(MigrationRunner::builtin().run(minusOne, 3).ok());
    CHECK(static_cast<int>(minusOne["project"]["scenes"][0]["tracks"][0]["midi"]["notes"][1]) == -1);
}

TEST_CASE("migration: every fixture version migrates to the current one", "[store][migration][codec]")
{
    const bool regenerate = juce::SystemStats::getEnvironmentVariable("APP_REGENERATE_FIXTURES", {}) == "1";
    const auto current = fixtureRoot().getChildFile("v" + juce::String(kSchemaVersion));
    if (regenerate)
    {
        SaveOptions pinned;
        pinned.timestamp = "2026-11-21T00:00:00Z";
        pinned.appName = "fixture-writer";
        pinned.appVersion = "1";
        for (const char* name : {"default", "beat", "random-42"})
        {
            const auto bundle = current.getChildFile(juce::String(name) + ".sixteen");
            current.createDirectory();
            bundle.deleteRecursively();
            REQUIRE(saveProject(*generateFixture(name), bundle, pinned).ok());
        }
        // The expected result of every older fixture: the fixture migrated and saved in the current schema.
        for (const auto& versionDir : fixtureRoot().findChildFiles(juce::File::findDirectories, false, "v*"))
            if (versionDir != current)
                for (const auto& bundle : versionDir.findChildFiles(juce::File::findDirectories, false, "*.sixteen"))
                {
                    auto migrated = std::unique_ptr<core::model::Project>(new core::model::Project);
                    ImportReport report;
                    REQUIRE(openProjectInto(bundle, *migrated, report).ok());
                    const auto out = fixtureRoot().getChildFile("expected").getChildFile(versionDir.getFileName())
                                         .getChildFile(bundle.getFileName());
                    out.getParentDirectory().createDirectory();
                    out.deleteRecursively();
                    REQUIRE(saveProject(*migrated, out, pinned).ok());
                }
    }

    int fixtures = 0;
    for (const auto& versionDir : fixtureRoot().findChildFiles(juce::File::findDirectories, false, "v*"))
        for (const auto& bundle : versionDir.findChildFiles(juce::File::findDirectories, false, "*.sixteen"))
        {
            INFO(bundle.getFullPathName());
            auto migrated = std::unique_ptr<core::model::Project>(new core::model::Project);
            ImportReport report;
            const auto r = openProjectInto(bundle, *migrated, report);
            REQUIRE(r.ok());
            CHECK_FALSE(report.readOnly);
            std::vector<std::string> problems;
            CHECK(core::model::validate(*migrated, problems));

            // expected/v<N>/<name>.sixteen when the fixture is older than the current version, else the fixture itself
            auto expectedBundle = fixtureRoot().getChildFile("expected").getChildFile(versionDir.getFileName())
                                      .getChildFile(bundle.getFileName());
            if (versionDir == current || ! expectedBundle.isDirectory())
            {
                REQUIRE(versionDir == current); // an old fixture needs its expected bundle
                expectedBundle = bundle;
            }
            auto expected = std::unique_ptr<core::model::Project>(new core::model::Project);
            ImportReport expectedReport;
            REQUIRE(NativeCodec::readInto(expectedBundle, *expected, expectedReport).ok());
            if (! (*migrated == *expected))
                FAIL_CHECK(storetest::firstDifference(*expected, *migrated));
            CHECK(*migrated == *expected);

            // write(read(x)) == x for current-version fixtures (meta.json timestamps aside)
            if (versionDir == current)
            {
                SaveOptions pinned;
                pinned.timestamp = "2026-11-21T00:00:00Z";
                pinned.appName = "fixture-writer";
                pinned.appVersion = "1";
                for (const auto& f : NativeCodec::encode(*migrated, pinned))
                {
                    INFO(f.name);
                    CHECK(storetest::readBytes(bundle.getChildFile(juce::String::fromUTF8(f.name.c_str()))) == f.text);
                }
                // and the generator still produces the same model (the defaults or the generator changed otherwise)
                const auto name = bundle.getFileNameWithoutExtension();
                if (name != "random-42") // std distributions differ between standard libraries
                    CHECK(*generateFixture(name) == *migrated);
            }
            ++fixtures;
        }
    CHECK(fixtures >= 3);
}
