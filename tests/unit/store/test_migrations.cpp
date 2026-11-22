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
