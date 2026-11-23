// Native format v1 (FMT-010…014, FMT-020): round trips (default project and 200 random seeds), sparse files, extras
// and unknown keys (forward compatibility), newer and damaged files.
#include "tests/unit/store/StoreTestSupport.h"

#include "services/store/CodecRegistry.h"
#include "services/store/NativeCodecSchema.h"

#include <catch2/catch_test_macros.hpp>

using namespace services::store;
using storetest::ScratchFolder;

namespace
{
std::unique_ptr<core::model::Project> readBack(const juce::File& bundle, ImportReport& report)
{
    auto p = std::unique_ptr<core::model::Project>(new core::model::Project);
    const auto r = NativeCodec::readInto(bundle, *p, report);
    INFO(r.error().message << " " << r.error().detail);
    REQUIRE(r.ok());
    return p;
}

const NativeCodec::EncodedFile& fileNamed(const std::vector<NativeCodec::EncodedFile>& files, std::string_view name)
{
    for (const auto& f : files)
        if (f.name == name)
            return f;
    FAIL("no encoded file " << name);
    return files.front();
}
} // namespace

TEST_CASE("store: the default project round-trips and stays small", "[store][codec]")
{
    ScratchFolder dir("default");
    const auto p = core::model::newDefaultProject();
    const auto bundle = dir / "Default.sixteen";
    SaveReport saved;
    const auto r = saveProject(*p, bundle, {}, &saved);
    INFO(r.error().message);
    REQUIRE(r.ok());
    CHECK(bundle.getChildFile("meta.json").existsAsFile());
    CHECK(bundle.getChildFile("project.json").existsAsFile());
    CHECK(bundle.getChildFile("patterns/scene-16.json").existsAsFile());
    UNSCOPED_INFO("default project: " << saved.bytesWritten << " bytes in 18 files");
    CHECK(saved.bytesWritten < 64 * 1024);

    ImportReport report;
    const auto back = readBack(bundle, report);
    if (! (*back == *p))
        FAIL_CHECK(storetest::firstDifference(*p, *back));
    CHECK(*back == *p);
    CHECK(report.warnings.empty());
    CHECK(report.unmapped.empty());
    CHECK_FALSE(report.readOnly);

    // the registry finds the codec and openProject() gives the same project
    REQUIRE(codecFor(bundle) != nullptr);
    CHECK(codecFor(bundle)->info().id == "native");
    CHECK(codecForExtension(".SIXTEEN") == findCodec("native"));
    auto opened = openProject(bundle);
    REQUIRE(opened.ok());
    CHECK(*opened.value() == *p);
}

TEST_CASE("store: read(write(p)) == p for 200 random projects", "[store][codec][property]")
{
    ScratchFolder dir("property");
    int checked = 0;
    for (std::uint32_t seed = 1; seed <= 200; ++seed)
    {
        const auto p = storetest::randomProject(seed);
        std::vector<std::string> problems;
        INFO("seed " << seed << ": " << (problems.empty() ? "" : problems.front()));
        REQUIRE(core::model::validate(*p, problems));

        const auto bundle = dir.get().getChildFile("p" + juce::String(seed) + ".sixteen");
        const auto saved = saveProject(*p, bundle);
        INFO(saved.error().message);
        REQUIRE(saved.ok());
        ImportReport report;
        const auto back = readBack(bundle, report);
        if (! (*back == *p))
            FAIL_CHECK(storetest::firstDifference(*p, *back));
        REQUIRE(*back == *p);
        CHECK(report.warnings.empty());
        // and (every 10th seed, to keep the Debug + ASan run short) the read-back project encodes to the same bytes
        if (seed % 10 != 0)
        {
            bundle.deleteRecursively();
            ++checked;
            continue;
        }
        SaveOptions pinned;
        pinned.timestamp = "2026-01-01T00:00:00Z";
        const auto a = NativeCodec::encode(*p, pinned), b = NativeCodec::encode(*back, pinned);
        REQUIRE(a.size() == b.size());
        for (std::size_t i = 0; i < a.size(); ++i)
            CHECK(a[i].text == b[i].text);
        bundle.deleteRecursively();
        ++checked;
    }
    CHECK(checked == 200);
}

TEST_CASE("store: writeDefaults files read back to the same project", "[store][codec]")
{
    ScratchFolder dir("full");
    const auto p = storetest::randomProject(77);
    SaveOptions full;
    full.writeDefaults = true;
    const auto bundle = dir / "Full.sixteen";
    REQUIRE(saveProject(*p, bundle, full).ok());
    ImportReport report;
    const auto back = readBack(bundle, report);
    if (! (*back == *p))
        FAIL_CHECK(storetest::firstDifference(*p, *back));
    CHECK(*back == *p);
}

TEST_CASE("store: sparse steps keep a one-pattern beat under 4 KB", "[store][codec]")
{
    auto p = core::model::newDefaultProject();
    auto& pattern = p->scenes[0].patterns[0];
    const auto hit = [&](int step, int track, int velocity)
    {
        auto& cell = pattern.steps[static_cast<std::size_t>(step)][static_cast<std::size_t>(track)];
        cell.trigger = 0;
        cell.value2 = static_cast<std::int16_t>(velocity);
    };
    for (int s = 0; s < 16; s += 4)
        hit(s, 0, 120); // kick
    hit(4, 1, 110);     // snare
    hit(12, 1, 110);
    for (int s = 0; s < 16; s += 2)
        hit(s, 2, s % 4 == 0 ? 100 : 70); // hats
    hit(14, 3, 90);
    // two p-locks
    core::model::StepLocks locks;
    locks.mask = (1u << 4) | (std::uint64_t {1} << 24);
    locks.values[4] = 300.0f;
    locks.values[24] = 0.42f;
    locks.change[4] = core::model::LockChange::Glide;
    pattern.steps[14][3].lockIndex = 0;
    pattern.locks.push_back(locks);

    const auto files = NativeCodec::encode(*p);
    const auto& scene = fileNamed(files, "patterns/scene-01.json");
    UNSCOPED_INFO("1-pattern beat (16 steps, 2 locks): " << scene.text.size() << " bytes\n" << scene.text);
    CHECK(scene.text.size() <= 4096);
    CHECK(fileNamed(files, "patterns/scene-02.json").text.size() < 64); // an empty scene is a stub
    CHECK(scene.text.find("\"pitch\": 300") != std::string::npos);
    CHECK(scene.text.find("\"fx1.p2\": 0.42") != std::string::npos);
    CHECK(scene.text.find("\"lockChange\": { \"pitch\": \"Glide\" }") != std::string::npos);

    ScratchFolder dir("beat");
    REQUIRE(saveProject(*p, dir / "Beat.sixteen").ok());
    ImportReport report;
    const auto back = readBack(dir / "Beat.sixteen", report);
    CHECK(*back == *p);
}

TEST_CASE("store: the file layout follows the spec's sketch", "[store][codec]")
{
    auto p = core::model::newDefaultProject();
    p->settings.tempo = 131.0;
    p->scenes[0].name = "Intro";
    p->scenes[0].sendFx.type = core::model::EffectType::Reverb;
    p->scenes[0].sendFx.params = {0.3, 0.5, 0.6, 0.4, 0.5, 0.35};
    p->scenes[0].sendFx.typeMemory.push_back({core::model::EffectType::Chorus, 1, 0, {0.25}, false, false});
    p->sampleRefs.entries.push_back({"sha256:" + std::string(64, 'a'), "$Lib/Drums/kick.wav", "kick", false, {}});
    p->scenes[0].tracks[0].playback.sample.id = p->sampleRefs.entries[0].id;
    SaveOptions o;
    o.timestamp = "2026-01-01T00:00:00Z";
    const auto files = NativeCodec::encode(*p, o);
    REQUIRE(files.size() == 18);
    const auto& project = fileNamed(files, "project.json").text;
    const auto& meta = fileNamed(files, "meta.json").text;
    CHECK(project.find("\"format\": { \"name\": \"sixteen-project\", \"version\": " + std::to_string(kSchemaVersion) +
                       " }") != std::string::npos);
    CHECK(project.find("\"tempo\": 131") != std::string::npos);
    CHECK(project.find("\"name\": \"Intro\"") != std::string::npos);
    CHECK(project.find("\"type\": \"Reverb\"") != std::string::npos);
    CHECK(project.find("\"params\": [0.3, 0.5, 0.6, 0.4, 0.5, 0.35]") != std::string::npos);
    CHECK(project.find("\"perType\": { \"Chorus\": { \"type1\": 1, \"params\": [0.25, 0, 0, 0, 0, 0] } }") !=
          std::string::npos); // FMT-014
    CHECK(meta.find("\"location\": \"$Lib\", \"path\": \"Drums/kick.wav\"") != std::string::npos); // FMT-011
    CHECK(meta.find("\"created\": \"2026-01-01T00:00:00Z\"") != std::string::npos);
    CHECK(project.find("samplePool") == std::string::npos); // the pool index is meta.json's
}

TEST_CASE("store: the Send / Master FX bypass is saved per scene (FX-046)", "[store][codec]")
{
    ScratchFolder dir("busbypass");
    auto p = core::model::newDefaultProject();
    p->scenes[5].masterFx.bypass = true;
    p->scenes[9].sendFx.bypass = true;
    const auto bundle = dir / "BusBypass.sixteen";
    REQUIRE(saveProject(*p, bundle, {}, nullptr).ok());
    ImportReport report;
    const auto back = readBack(bundle, report);
    CHECK(back->scenes[5].masterFx.bypass);
    CHECK_FALSE(back->scenes[5].sendFx.bypass);
    CHECK(back->scenes[9].sendFx.bypass);
    CHECK(*back == *p);
}

TEST_CASE("store: unknown keys, unknown enum strings and foreign extras survive a round trip", "[store][codec]")
{
    ScratchFolder dir("future");
    const auto bundle = dir / "Future.sixteen";
    auto p = core::model::newDefaultProject();
    REQUIRE(saveProject(*p, bundle).ok());

    // a newer writer adds keys and an enum value this build does not know
    auto root = NativeCodec::load(bundle);
    REQUIRE(root.ok());
    auto project = root.value()["project"];
    project.getDynamicObject()->setProperty("futureTopLevel", juce::JSON::parse("{\"a\":[1,2.5,\"x\"]}"));
    auto track = project["scenes"][2]["tracks"][5];
    track.getDynamicObject()->setProperty("mode", "Granular");
    track.getDynamicObject()->setProperty("fadeIn", 12);
    auto lfo = new juce::DynamicObject();
    lfo->setProperty("wobble", true);
    track.getDynamicObject()->setProperty("lfo", juce::var(lfo));
    std::string text;
    detail::JsonWriter w(text);
    w.var(project);
    REQUIRE(juce::File(bundle.getChildFile("project.json")).replaceWithText(juce::String::fromUTF8(text.c_str())));

    ImportReport report;
    const auto back = readBack(bundle, report);
    const auto& t = back->scenes[2].tracks[5];
    CHECK(t.mode == core::model::TrackMode::Sample); // default kept
    REQUIRE(t.extras.find("native.mode") != nullptr);
    CHECK(t.extras.find("native.mode")->encoding == core::model::Extras::Encoding::Text);
    CHECK(t.extras.find("native.mode")->value == "Granular");
    REQUIRE(t.extras.find("native.fadeIn") != nullptr);
    CHECK(t.extras.find("native.fadeIn")->value == "12");
    CHECK(t.lfo.extras.find("native.wobble")->value == "true");
    REQUIRE(back->extras.find("native.futureTopLevel") != nullptr);
    CHECK(back->extras.find("native.futureTopLevel")->value == "{\"a\":[1,2.5,\"x\"]}");
    CHECK_FALSE(report.warnings.empty()); // the unknown enum is reported

    // written back where they were
    const auto files = NativeCodec::encode(*back);
    const auto& out = fileNamed(files, "project.json").text;
    CHECK(out.find("\"mode\": \"Granular\"") != std::string::npos);
    CHECK(out.find("\"fadeIn\": 12") != std::string::npos);
    CHECK(out.find("\"futureTopLevel\": {\"a\":[1,2.5,\"x\"]}") != std::string::npos);
    REQUIRE(saveProject(*back, bundle).ok());
    ImportReport again;
    const auto twice = readBack(bundle, again);
    if (! (*twice == *back))
        FAIL_CHECK(storetest::firstDifference(*back, *twice));
    CHECK(*twice == *back);
}

TEST_CASE("store: a newer format opens read-only, damaged and foreign files fail cleanly", "[store][codec]")
{
    ScratchFolder dir("versions");
    const auto bundle = dir / "New.sixteen";
    const auto p = core::model::newDefaultProject();
    REQUIRE(saveProject(*p, bundle).ok());
    const auto meta = bundle.getChildFile("meta.json");

    meta.replaceWithText(meta.loadFileAsString().replace("\"version\": " + juce::String(kSchemaVersion),
                                                         "\"version\": 99"));
    ImportReport report;
    const auto newer = readBack(bundle, report);
    CHECK(report.readOnly); // PRJ-052
    CHECK(*newer == *p);

    meta.replaceWithText(meta.loadFileAsString().replace("sixteen-project", "something-else"));
    ImportReport r2;
    auto other = core::model::newDefaultProject();
    CHECK_FALSE(NativeCodec::readInto(bundle, *other, r2).ok());

    bundle.getChildFile("project.json").replaceWithText("{ \"settings\": ");
    ImportReport r3;
    const auto damaged = NativeCodec::readInto(bundle, *other, r3);
    REQUIRE_FALSE(damaged.ok());
    CHECK(damaged.error().message.find("damaged") != std::string::npos);
    CHECK(damaged.error().message.find("project.json") != std::string::npos);

    CHECK_FALSE(openProject(dir / "Missing.sixteen").ok());
}

TEST_CASE("store: wrong types and out-of-range addresses are warnings, not failures", "[store][codec]")
{
    ScratchFolder dir("lenient");
    const auto bundle = dir / "Odd.sixteen";
    REQUIRE(saveProject(*core::model::newDefaultProject(), bundle).ok());
    bundle.getChildFile("patterns/scene-03.json")
        .replaceWithText("{ \"scene\": 3, \"patterns\": [ { \"index\": 2, \"length\": \"long\", \"steps\": ["
                         "{ \"track\": 17, \"step\": 1 }, { \"track\": 1, \"step\": 2, \"trigger\": 0, "
                         "\"locks\": { \"pitch\": 100, \"nosuch\": 1 } } ] }, { \"index\": 40 } ] }");
    ImportReport report;
    const auto back = readBack(bundle, report);
    const auto& pattern = back->scenes[2].patterns[1];
    CHECK(pattern.length == 16);
    CHECK(pattern.steps[1][0].trigger == 0);
    REQUIRE(pattern.locks.size() == 1);
    CHECK(pattern.locks[0].mask == (1u << 4));
    CHECK(pattern.locks[0].values[4] == 100.0f);
    CHECK(report.warnings.size() >= 4);
}

TEST_CASE("store: the current schema's header defaults are pinned by a fixture", "[store][codec]")
{
    // Omitted members read as these defaults: a change here needs a migration (Migrations.h), never a silent edit.
    // v<N>-defaults.json per schema version (v1-defaults.json stays: what a v1 writer omitted).
    static const auto fixture = juce::File(APP_SOURCE_DIR)
                                    .getChildFile("tests/fixtures/native/v" + juce::String(kSchemaVersion) + "-defaults.json");
    std::string text;
    {
        detail::JsonWriter w(text);
        detail::Encoder enc(w, true);
        w.beginObject();
        enc.pushFrame();
        const auto& d = detail::defaultProject();
        enc.object("settings", d.settings, d.settings, detail::Layout::Lines);
        enc.object("scene", d.scenes[0], d.scenes[0], detail::Layout::Lines); // its 16 tracks too
        enc.object("arrangement", d.arrangement, d.arrangement, detail::Layout::Lines);
        enc.object("section", core::model::Section {}, core::model::Section {}, detail::Layout::Lines);
        enc.object("ribbon", d.ribbon, d.ribbon, detail::Layout::Lines);
        enc.object("midi", d.midi, d.midi, detail::Layout::Lines);
        enc.object("origin", d.origin, d.origin, detail::Layout::Lines);
        enc.object("pattern", detail::defaultPatternValue(), detail::defaultPatternValue(), detail::Layout::Lines);
        enc.object("sampleRef", core::model::SampleRef {}, core::model::SampleRef {}, detail::Layout::Lines);
        enc.popFrame();
        w.endObject();
        text += '\n';
    }
    if (juce::SystemStats::getEnvironmentVariable("APP_REGENERATE_FIXTURES", {}) == "1")
        storetest::writeBytes(fixture, text);
    REQUIRE(fixture.existsAsFile());
    CHECK(storetest::readBytes(fixture) == text);
}
