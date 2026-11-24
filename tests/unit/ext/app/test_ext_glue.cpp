// The composition root's extension glue (app/ext/Extensions.h; flags and parity): with every ext.* flag off it posts,
// writes and logs nothing (the app without extensions); with a flag on it forwards TOGGLE, applies and stores learned
// CCs through the model path (recorded strip moves: app/Document, tests/unit/ext/striprecord).
#include "app/ext/Extensions.h"

#include "core/commands/CommandBusAccess.h"
#include "core/commands/ext/ExtCommands.h"
#include "core/model/ModelOps.h"
#include "core/params/ParamRegistry.h"
#include "services/ext/ExtFlags.h"
#include "services/settings/Preferences.h"
#include "tests/fakes/FakeEngineBridge.h"
#include "ui/descriptors/SettingsSpec.h"

#include <catch2/catch_test_macros.hpp>

#include <map>

namespace
{
core::ParamId id(std::string_view name) { return core::builtinParams().idOf(name).value_or(core::ParamId {}); }

struct Rig
{
    core::CommandBus bus;
    fakes::FakeEngineBridge bridge;
    std::vector<std::string> logs;
    std::vector<std::pair<std::string, float>> remote;
    std::optional<app::ext::Extensions> ext;

    Rig()
    {
        services::ext::flags().clearOverrides();
        auto p = core::model::newDefaultProject();
        p->scenes[0].tracks[0].extras.set("native.ext.trk.toggle", "true");
        core::commands::replaceProject(bus, std::move(p));
        core::commands::setBridge(bus, [this](const core::ChangeSet& c, const core::model::Project& pr) {
            if (ext)
                ext->onChange(c, pr);
        });
        ext.emplace(app::ext::ExtEnv {
            .bus = &bus,
            .bridge = &bridge,
            .log = [this](const std::string& l) { logs.push_back(l); },
            .setRemote = [this](const core::ParamSpec& s, std::uint16_t, float v) { remote.emplace_back(std::string(s.id), v); },
            .selectedTrack = [] { return 2; },
            .resolveSample = [](const std::string&) { return std::optional<juce::File>(); },
            .preference = {}});
    }
    ~Rig()
    {
        ext.reset();
        core::commands::setBridge(bus, {});
        services::ext::flags().clearOverrides();
    }
    const core::model::Project& project() { return core::commands::project(bus); }
    void frame() { ext->frame(0); }
    int posts(std::string_view param, std::uint16_t scope, float value) const
    {
        int n = 0;
        for (const auto& m : bridge.posted)
            if (const auto* c = std::get_if<engine::ParamChange>(&m))
                n += c->param == id(param) && c->scope == scope && c->value == value ? 1 : 0;
        return n;
    }
    bool command(const std::string& text)
    {
        std::istringstream in(text);
        std::string cmd;
        in >> cmd;
        return ext->command(cmd, in);
    }
};

core::midi::UiEvent carrierEvent(int track, int cc, int value)
{
    core::midi::UiEvent u;
    u.kind = core::midi::UiEvent::Kind::Param;
    u.scope = static_cast<std::uint16_t>(track);
    u.param = id(services::midilearn::kCarrierId).value;
    u.value = static_cast<float>(cc * 128 + value);
    return u;
}
} // namespace

TEST_CASE("ext glue: with every flag off nothing is posted, written or logged", "[ext][glue][parity]")
{
    Rig r;
    REQUIRE_FALSE(services::ext::flags().isOn(services::ext::kMaster)); // the test environment sets no ext.* flag
    r.ext->onProjectReplaced(r.project());
    r.frame();
    CHECK_FALSE(r.ext->onMidiUi(carrierEvent(0, 85, 10)));
    r.frame();
    CHECK(r.bridge.posted.empty());
    CHECK(r.remote.empty());
    CHECK(r.logs.empty());
    // Commands answer, but change nothing.
    const auto before = r.project();
    CHECK(r.command("ext-toggle 2 on"));
    CHECK(r.command("ext-learn track.pan 1"));
    CHECK(r.command("ext-export /tmp/never-written Smoke"));
    CHECK(r.project() == before);
    CHECK(r.bridge.posted.empty());
    CHECK_FALSE(r.command("not-ext"));
    CHECK_FALSE(juce::File("/tmp/never-written").exists());
}

TEST_CASE("ext glue: TOGGLE follows the flag, the model and undo", "[ext][glue][toggle]")
{
    Rig r;
    services::ext::flags().set(services::ext::kMaster, true);
    services::ext::flags().set(services::ext::kToggle, true);
    r.frame();
    CHECK(r.posts("ext.toggle.track", 0x00, 1.0f) == 1); // the model's TOGGLE on scene 1 track 1
    CHECK(r.command("ext-toggle 2 on"));
    CHECK(r.project().scenes[0].tracks[1].extras.find("native.ext.trk.toggle") != nullptr);
    CHECK(r.posts("ext.toggle.track", 0x01, 1.0f) == 1);
    CHECK(r.bus.undo());
    CHECK(r.project().scenes[0].tracks[1].extras.find("native.ext.trk.toggle") == nullptr);
    CHECK(r.posts("ext.toggle.track", 0x01, 0.0f) == 1);
    // The flag off: every posted TOGGLE back to 0; the model keeps its value (a file written with it keeps it).
    services::ext::flags().set(services::ext::kToggle, false);
    r.frame();
    CHECK(r.posts("ext.toggle.track", 0x00, 0.0f) == 1);
    CHECK(r.project().scenes[0].tracks[0].extras.find("native.ext.trk.toggle") != nullptr);
}

TEST_CASE("ext glue: MIDI learn captures, stores per project (undoable) and applies through the model path",
          "[ext][glue][midilearn]")
{
    Rig r;
    services::ext::flags().set(services::ext::kMaster, true);
    services::ext::flags().set(services::ext::kMidiLearn, true);
    r.frame();
    CHECK(r.command("ext-learn track.volume 1"));
    CHECK(r.ext->onMidiUi(carrierEvent(0, 85, 10))); // track 1 receives channel 1
    const auto* stored = r.project().extras.find(std::string(services::midilearn::kExtrasKey));
    REQUIRE(stored != nullptr);
    CHECK(stored->value.find("track.volume") != std::string::npos);
    CHECK(r.remote.empty()); // the learning CC itself sets nothing
    CHECK(r.ext->onMidiUi(carrierEvent(0, 85, 127)));
    REQUIRE(r.remote.size() == 1);
    CHECK(r.remote[0] == std::make_pair(std::string("track.volume"), 127.0f));
    // Another channel (track 2 receives channel 2): no binding.
    CHECK(r.ext->onMidiUi(carrierEvent(1, 85, 127)));
    CHECK(r.remote.size() == 1);
    // Undo removes the binding from the project and the live map.
    CHECK(r.bus.undo());
    CHECK(r.project().extras.find(std::string(services::midilearn::kExtrasKey)) == nullptr);
    CHECK(r.ext->onMidiUi(carrierEvent(0, 85, 0)));
    CHECK(r.remote.size() == 1);
}

TEST_CASE("ext glue: the flags come from the preferences (UTILITY rows), an environment value still wins",
          "[ext][glue][settings]")
{
    std::map<std::string, float, std::less<>> prefs;
    {
        Rig r; // no preference source: environment only
        r.ext.reset();
        r.ext.emplace(app::ext::ExtEnv {.bus = &r.bus,
                                        .bridge = &r.bridge,
                                        .log = [&](const std::string& l) { r.logs.push_back(l); },
                                        .preference = [&](std::string_view key) {
                                            const auto it = prefs.find(key);
                                            return it != prefs.end() ? it->second : 0.0f;
                                        }});
        r.frame();
        CHECK(r.bridge.posted.empty()); // preferences at their defaults: off
        prefs["ext.enabled"] = 1.0f;
        CHECK_FALSE(services::ext::flags().isOn(services::ext::kToggle));
        prefs["ext.trk.toggle"] = 1.0f; // UTILITY › EXTENSIONS › TOGGLE TRIGGER MODE
        CHECK(services::ext::flags().isOn(services::ext::kToggle));
        r.frame();
        CHECK(r.posts("ext.toggle.track", 0x00, 1.0f) == 1);
        prefs["ext.enabled"] = 0.0f; // SIXTEEN EXTENSIONS off: everything off
        r.frame();
        CHECK(r.posts("ext.toggle.track", 0x00, 0.0f) == 1);
    }
    // The glue gone: the environment-only source is back.
    prefs["ext.enabled"] = 1.0f;
    prefs["ext.trk.toggle"] = 1.0f;
    CHECK_FALSE(services::ext::flags().isOn(services::ext::kToggle));
}

TEST_CASE("ext glue: the UTILITY sections EXTENSIONS and MIDI LEARN sit before INFORMATION, rows behind ext.enabled",
          "[ext][glue][settings]")
{
    const auto& reg = ui::SettingsRegistry::builtin();
    int ext = -1, learn = -1, info = -1, rows = 0;
    for (const auto& s : reg.sections("utility"))
    {
        if (s.id == "extensions")
            ext = s.order;
        if (s.id == "midiLearn")
            learn = s.order;
        if (s.id == "information")
            info = s.order;
        if (s.id == "extensions" || s.id == "midiLearn")
            for (const auto& row : reg.rows("utility", s.id))
            {
                ++rows;
                CHECK(row.featureFlag == "ext.enabled");
                CHECK(services::settings::PrefRegistry::builtin().find(row.binds) != nullptr);
            }
    }
    CHECK(ext > 0);
    CHECK(learn > ext);
    CHECK(info > learn);
    CHECK(rows == 4);
}
