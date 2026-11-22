// services::settings::Preferences (SET-010…013, UI-325) — load / migrate / save, the atomic write and
// a kill -9 during saves, the debounce (≤ 1 s), export / import with controller profiles (SET-012), feature flags
// (FX-001), what counts as a preference, and the shell-side PreferenceAccess / applyPreferences.
#include "core/params/ParamRegistry.h"
#include "services/settings/Preferences.h"
#include "ui/screens/utility/PrefAccess.h"
#include "ui/shell/ParamAccess.h"

#include <catch2/catch_test_macros.hpp>

#include <juce_core/juce_core.h>

#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace st = services::settings;

namespace
{
struct TempDir
{
    juce::File dir;
    TempDir()
    {
        dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                  .getChildFile("settings-test-" + juce::String(::getpid()) + "-" + juce::String(juce::Random::getSystemRandom().nextInt64()));
        dir.createDirectory();
    }
    ~TempDir() { dir.deleteRecursively(); }
    juce::File file(const char* name = "preferences.json") const { return dir.getChildFile(name); }
};

juce::var parse(const juce::File& f)
{
    juce::var v;
    REQUIRE(juce::JSON::parse(f.loadFileAsString(), v).wasOk());
    return v;
}
} // namespace

TEST_CASE("settings: a missing file is the defaults, get() falls back to the spec's default", "[settings]")
{
    TempDir t;
    st::Preferences p;
    REQUIRE(p.load(t.file()));
    CHECK(p.values().empty());
    CHECK(p.get("project.resume") == 1.0f);            // PRJ-022 default ON
    CHECK(p.get("prefs.pads.velocityCurve") == 3.0f);  // pads.velocityCurve's storage key: LINEAR
    CHECK(p.get("audio.inputLatency") == 0.0f);        // live.inputLatency's storage key
    CHECK(p.get("ribbon.applyTo") == 0.0f);
    CHECK(p.get("settings.autosave") == 1.0f);         // 1 min
    CHECK(p.get("ext.enabled") == 0.0f);               // PrefSpec
    CHECK(p.get("no.such.key") == 0.0f);
    CHECK_FALSE(p.dirty());
}

TEST_CASE("settings: set / flush / load round-trips, the file is version 1, sorted, atomic", "[settings]")
{
    TempDir t;
    {
        st::Preferences p;
        REQUIRE(p.load(t.file()));
        p.set("project.resume", 0);
        p.set("prefs.pads.velocityCurve", 5);
        p.set("midi.portMode#2", 3);           // an unknown key is kept as is
        p.set("audio.inputLatency", 12.6f);    // integer spec: rounded
        p.set("ui.scale", 900);                // clamped to 200
        p.setString("audio.deviceName", "hw:0");
        p.setList("library.locations", {"/media/a", "/media/b"});
        CHECK(p.dirty());
        p.flush();
        CHECK_FALSE(p.dirty());
        CHECK(p.writes() == 1);
    }
    const auto v = parse(t.file());
    CHECK(static_cast<int>(v["version"]) == st::Preferences::kSchemaVersion);
    CHECK(v["format"].toString().endsWith("-preferences"));
    CHECK(static_cast<int>(v["values"]["project.resume"]) == 0);
    CHECK(static_cast<int>(v["values"]["ui.scale"]) == 200);
    CHECK(static_cast<int>(v["values"]["audio.inputLatency"]) == 13);
    // No temp file left next to it (temp → fsync → rename).
    CHECK(t.dir.findChildFiles(juce::File::findFiles, false, "*.tmp-*").isEmpty());

    st::Preferences q;
    REQUIRE(q.load(t.file()));
    CHECK(q.get("project.resume") == 0.0f);
    CHECK(q.get("prefs.pads.velocityCurve") == 5.0f);
    CHECK(q.get("midi.portMode#2") == 3.0f);
    CHECK(q.getString("audio.deviceName") == "hw:0");
    CHECK(q.getList("library.locations") == std::vector<std::string> {"/media/a", "/media/b"});
    CHECK_FALSE(q.dirty()); // a current-version file is not rewritten
    // The same values give the same bytes (reproducible saves).
    CHECK(q.toJson() == t.file().loadFileAsString().toStdString());
}

TEST_CASE("settings: schema 0 is migrated, unknown members survive, a newer version keeps what it understands",
          "[settings]")
{
    TempDir t;
    SECTION("schema 0 (flat object)")
    {
        t.file().replaceWithText(R"({"project.resume": 0, "ui.theme": 1, "audio.deviceName": "card"})");
        st::Preferences p;
        REQUIRE(p.load(t.file()));
        CHECK(p.get("project.resume") == 0.0f);
        CHECK(p.get("ui.theme") == 1.0f);
        CHECK(p.getString("audio.deviceName") == "card");
        CHECK(p.dirty()); // rewritten in schema 1 on the next tick
        p.tick(0);
        p.tick(2000);
        CHECK(p.writes() == 1);
        CHECK(static_cast<int>(parse(t.file())["version"]) == 1);
    }
    SECTION("schema 2 with an unknown section")
    {
        t.file().replaceWithText(R"({"format": "x-preferences", "version": 2, "values": {"project.resume": 0,
                                    "future.key": 7}, "cloud": {"sync": true}})");
        st::Preferences p;
        REQUIRE(p.load(t.file()));
        CHECK(p.get("project.resume") == 0.0f);
        CHECK(p.get("future.key") == 7.0f);
        p.set("ui.theme", 1);
        p.flush();
        const auto v = parse(t.file());
        CHECK(static_cast<bool>(v["cloud"]["sync"]));
        CHECK(static_cast<int>(v["values"]["future.key"]) == 7);
    }
}

TEST_CASE("settings: a corrupt file is moved aside and the defaults are used", "[settings]")
{
    TempDir t;
    t.file().replaceWithText("{\"version\": 1, \"values\": {\"project.resume\": 0");
    st::Preferences p;
    const auto r = p.load(t.file());
    CHECK_FALSE(r);
    CHECK(p.get("project.resume") == 1.0f);
    CHECK(t.file("preferences.json.corrupt").existsAsFile());
    CHECK_FALSE(t.file().existsAsFile());
    p.set("project.resume", 0);
    p.flush();
    CHECK(t.file().existsAsFile());
}

TEST_CASE("settings: saves are debounced 250 ms and never later than 1 s while values keep changing", "[settings]")
{
    TempDir t;
    st::Preferences p;
    REQUIRE(p.load(t.file()));
    p.tick(1000);
    p.set("project.resume", 0);
    p.tick(1100);
    CHECK(p.writes() == 0);
    p.tick(1260);
    CHECK(p.writes() == 1); // 250 ms after the change
    CHECK_FALSE(p.dirty());
    // A rotary turned continuously: one change every 100 ms; a write within 1 s of the first, not one per detent.
    double now = 2000;
    for (int i = 0; i < 25; ++i, now += 100)
    {
        p.tick(now);
        p.set("transport.metronomeVolume", static_cast<float>(i));
    }
    p.tick(now);
    CHECK(p.writes() >= 3);
    CHECK(p.writes() <= 4); // 2.5 s of turning: ≈ one write per second
    p.tick(now + 300);
    CHECK_FALSE(p.dirty());
    st::Preferences q;
    REQUIRE(q.load(t.file()));
    CHECK(q.get("transport.metronomeVolume") == 24.0f);
    // Unchanged values do not save.
    const int before = p.writes();
    p.set("transport.metronomeVolume", 24);
    p.tick(now + 2000);
    CHECK(p.writes() == before);
}

TEST_CASE("settings: the write runs on the executor (a worker), one at a time", "[settings]")
{
    TempDir t;
    std::vector<std::thread> threads;
    std::atomic<int> jobs {0};
    {
        st::Preferences p(core::builtinParams(), [&](std::function<void()> job) {
            ++jobs;
            threads.emplace_back([job = std::move(job)] {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                job();
            });
        });
        REQUIRE(p.load(t.file()));
        p.tick(0);
        p.set("ui.theme", 1);
        p.tick(300);
        CHECK(p.writing());
        p.set("ui.theme", 0); // during the write: stays dirty, written by the next tick
        p.tick(600);
        CHECK(jobs == 1);     // never two in flight
        p.flush();            // waits for the job, then writes synchronously
        CHECK_FALSE(p.writing());
        CHECK(p.writes() == 2);
    }
    for (auto& th : threads)
        th.join();
    st::Preferences q;
    REQUIRE(q.load(t.file()));
    CHECK(q.get("ui.theme") == 0.0f);
}

TEST_CASE("settings: kill -9 during saves leaves a valid file (the old or the new one)", "[settings]")
{
    TempDir t;
    {
        st::Preferences p;
        REQUIRE(p.load(t.file()));
        p.set("transport.metronomeVolume", 1);
        p.flush();
    }
    for (int round = 0; round < 5; ++round)
    {
        const pid_t child = ::fork();
        REQUIRE(child >= 0);
        if (child == 0)
        {
            // The child saves as fast as it can until it is killed.
            st::Preferences p;
            (void)p.load(t.file());
            for (int i = 0;; ++i)
            {
                p.set("transport.metronomeVolume", static_cast<float>(i % 128));
                p.setString("pad", std::string(static_cast<std::size_t>(1000 + i % 5000), 'x'));
                p.flush();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20 + 15 * round));
        ::kill(child, SIGKILL);
        int status = 0;
        ::waitpid(child, &status, 0);
        CHECK(WIFSIGNALED(status));
        st::Preferences q;
        const auto r = q.load(t.file()); // also removes a leftover temp file
        INFO(r.error().detail);
        CHECK(r);
        CHECK(q.get("transport.metronomeVolume") >= 0.0f);
        CHECK(t.dir.findChildFiles(juce::File::findFiles, false, "*.tmp-*").isEmpty());
        CHECK_FALSE(t.file("preferences.json.corrupt").existsAsFile());
    }
}

TEST_CASE("settings: observers see changes under their prefix", "[settings]")
{
    st::Preferences p;
    std::vector<std::string> seen;
    auto sub = p.observe("ui.", [&](std::string_view k) { seen.emplace_back(k); });
    p.set("ui.theme", 1);
    p.set("project.resume", 0);
    p.set("ui.theme", 1); // unchanged
    CHECK(seen == std::vector<std::string> {"ui.theme"});
    sub.reset();
    p.set("ui.theme", 0);
    CHECK(seen.size() == 1);
}

TEST_CASE("settings: export / import carries the preferences and the controller profiles (SET-012)", "[settings]")
{
    TempDir t;
    const auto profiles = t.dir.getChildFile("Controllers");
    profiles.createDirectory();
    profiles.getChildFile("generic-16.json").replaceWithText(R"({"name": "Generic 16", "pads": 16})");
    st::Preferences a;
    REQUIRE(a.load(t.file()));
    a.setProfilesFolder(profiles);
    a.set("project.resume", 0);
    a.set("prefs.pads.velocityCurve", 1);
    const auto out = t.file("export.json");
    REQUIRE(a.exportTo(out));

    TempDir u;
    const auto profiles2 = u.dir.getChildFile("Controllers");
    st::Preferences b;
    REQUIRE(b.load(u.file()));
    b.setProfilesFolder(profiles2);
    b.set("ui.theme", 1); // not in the export: back to the default
    std::vector<std::string> seen;
    auto sub = b.observe("", [&](std::string_view k) { seen.emplace_back(k); });
    REQUIRE(b.importFrom(out));
    CHECK(b.get("project.resume") == 0.0f);
    CHECK(b.get("prefs.pads.velocityCurve") == 1.0f);
    CHECK(b.get("ui.theme") == 0.0f);
    CHECK(std::find(seen.begin(), seen.end(), "project.resume") != seen.end());
    CHECK(profiles2.getChildFile("generic-16.json").existsAsFile());
    juce::var profile;
    REQUIRE(juce::JSON::parse(profiles2.getChildFile("generic-16.json").loadFileAsString(), profile).wasOk());
    CHECK(static_cast<int>(profile["pads"]) == 16);
    // Not an export: refused, nothing changed.
    t.file("other.json").replaceWithText("{\"version\": 1}");
    CHECK_FALSE(b.importFrom(t.file("other.json")));
    CHECK(b.get("project.resume") == 0.0f);
}

TEST_CASE("settings: feature flags follow ext.* under the ext.enabled master switch (FX-001)", "[settings]")
{
    st::Preferences p;
    st::FeatureFlags f(p);
    CHECK(f.isOn(""));
    p.set("ext.tremolo", 1);
    CHECK_FALSE(f.isOn("tremolo")); // master off: the reference set
    p.set("ext.enabled", 1);
    CHECK(f.isOn("tremolo"));
    CHECK(f.isOn("ext.tremolo"));
    CHECK_FALSE(f.isOn("vinylBrake"));
    CHECK(f.enabled() == std::vector<std::string> {"tremolo"});
}

TEST_CASE("settings: what is a preference — Global, not session./internal., not a display, machine settings",
          "[settings]")
{
    const auto& reg = core::builtinParams();
    auto persists = [&](std::string_view id) {
        const auto* s = reg.find(id);
        REQUIRE(s != nullptr);
        return st::persists(*s);
    };
    CHECK(persists("project.resume"));
    CHECK(persists("project.autosave"));
    CHECK(persists("pads.velocityCurve"));     // Global
    CHECK(persists("live.inputLatency"));      // Global
    CHECK(persists("ribbon.applyTo"));
    CHECK(persists("audio.stretchBackend"));   // Global
    CHECK(persists("transport.metronomeVolume")); // machine setting (Project scope, no model field)
    CHECK_FALSE(persists("pads.mode"));        // session.
    CHECK_FALSE(persists("render.muteLive"));  // internal.
    CHECK_FALSE(persists("browse.info"));      // display only
    CHECK_FALSE(persists("transport.tempo"));  // a project value
    CHECK(st::keyOf(*reg.find("pads.velocityCurve")) == "prefs.pads.velocityCurve");
    CHECK(st::keyOf(*reg.find("ui.theme"), 3) == "ui.theme#3");
    // Every persisted ParamSpec's key is unique (two specs never share a file entry).
    std::vector<std::string> keys;
    for (const auto& s : reg.all())
        if (st::persists(s))
            keys.emplace_back(s.storageKey());
    std::sort(keys.begin(), keys.end());
    CHECK(std::adjacent_find(keys.begin(), keys.end()) == keys.end());
}

TEST_CASE("settings: PreferenceAccess saves every preference edit, applyPreferences restores them", "[settings]")
{
    const auto& reg = core::builtinParams();
    st::Preferences prefs;
    ui::LocalParamValues local(reg);
    ui::utility::PreferenceAccess access(local, prefs);
    access.set(*reg.find("pads.velocityCurve"), 0, 6);
    access.set(*reg.find("browse.previewVolume"), 0, 80);
    access.set(*reg.find("pads.mode"), 0, 2);         // session state: not saved
    access.set(*reg.find("transport.tempo"), 0, 99);  // project value: not a preference
    CHECK(prefs.get("prefs.pads.velocityCurve") == 6.0f);
    CHECK(prefs.get("browse.previewVolume") == 80.0f);
    CHECK_FALSE(prefs.has("session.pads.mode"));
    CHECK_FALSE(prefs.has("transport.tempo"));
    CHECK(local.get(*reg.find("pads.velocityCurve"), 0) == 6.0f);

    ui::LocalParamValues fresh(reg);
    CHECK(ui::utility::applyPreferences(prefs, fresh) == 2);
    CHECK(fresh.get(*reg.find("pads.velocityCurve"), 0) == 6.0f);
    CHECK(fresh.get(*reg.find("browse.previewVolume"), 0) == 80.0f);
}

TEST_CASE("settings: the PrefSpec registry mirrors each spec as a Global ParamSpec", "[settings]")
{
    const auto& r = st::PrefRegistry::builtin();
    const auto* ext = r.find("ext.enabled");
    REQUIRE(ext != nullptr);
    const auto* p = r.paramOf("ext.enabled");
    REQUIRE(p != nullptr);
    CHECK(p->scope == core::ParamScope::Global);
    CHECK(p->enumLabels.size() == 2);
    char buf[16];
    CHECK(std::string_view(buf, static_cast<std::size_t>(p->formatValue(1, buf, sizeof buf))) == "ON");
    // A PrefSpec key never collides with a ParamSpec id.
    for (const auto* s : r.all())
        CHECK(core::builtinParams().find(s->key) == nullptr);
}
