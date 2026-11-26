#include "app/PluginAdapter.h"
#include "core/commands/CommandBusAccess.h"
#include "core/commands/fx/FxCommands.h"
#include "services/plugin/ClapHost.h"
#include "tests/unit/engine/EngineTestUtil.h"
#include "tests/unit/store/StoreTestSupport.h"
#include "ui/screens/effects/ModelFxSlots.h"

#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_CASE("CLAP FX discovery and rendering respect descriptor roles and port layouts", "[plugin][fx]")
{
    using namespace services::plugin;
    const auto entries = scanPluginDirectory(CLAP_FIXTURE_DIR);
    REQUIRE(entries.size() == 4);
    for (const auto& e : entries)
    {
        CAPTURE(e.id);
        const bool mono = e.id == "test.mono";
        const bool usable = mono || e.id == "test.stereo";
        CHECK(e.effectCompatible == usable);
        CHECK(e.trackCompatible == (e.id == "test.instrument"));
        std::string error;
        std::unique_ptr<ClapAudioNode> node(ClapAudioNode::create(e.absPath, e.id, 44100, error, true));
        REQUIRE(static_cast<bool>(node) == usable);
        if (! node)
            continue;
        std::array<float, 64> left{}, right{};
        // The effect asked to sleep on a silent block; fresh input while stopped must still be processed.
        node->processEffect({}, left.data(), right.data());
        left.fill(0.75f);
        right.fill(-0.25f);
        node->processEffect({}, left.data(), right.data());
        for (int i = 0; i < 64; ++i)
        {
            CHECK(left[i] == (mono ? 0.0625f : 0.1875f));
            CHECK(right[i] == (mono ? 0.0625f : -0.0625f));
        }
    }
}

TEST_CASE("CLAP instrument audio uses its declared input count and duplicates mono output", "[plugin]")
{
    using namespace services::plugin;
    std::string error;
    const auto path = juce::File(CLAP_FIXTURE_DIR).getChildFile("clap_effect_fixture.clap");
    std::unique_ptr<ClapAudioNode> node(
        ClapAudioNode::create(path.getFullPathName().toStdString(), "test.instrument", 44100, error));
    REQUIRE(node);
    std::array<float, 64> left{}, right{};
    node->noteOn(0, 60, 1.0f);
    REQUIRE(node->process({}, left.data(), right.data()));
    CHECK(left.front() == 0.25f);
    CHECK(right == left);
}

TEST_CASE("FX slots restore the exact descriptor on undo and native project load", "[plugin][fx][store]")
{
    using namespace core::model;
    core::CommandBus bus;
    core::commands::replaceProject(bus, newDefaultProject());
    const auto at = ui::fx::pathOf(ui::fx::Slot::Fx2, 4, 12);
    PluginTrack a{.path = "bundle.clap", .pluginId = "test.stereo", .name = "Stereo", .stateBase64 = "AAEC"};
    PluginTrack b{.path = a.path, .pluginId = "test.mono", .name = "Mono"};
    auto slot = [&]() -> const EffectSlot& { return core::fxSlotAt(core::commands::project(bus), at); };
    bus.execute(std::make_unique<core::SelectFxType>(at, EffectType::Plugin, a));
    bus.execute(std::make_unique<core::SelectFxType>(at, EffectType::Plugin, b));
    REQUIRE(slot().plugin == b);
    REQUIRE(bus.undo());
    REQUIRE(slot().type == EffectType::Plugin);
    CHECK(slot().plugin == a);
    storetest::ScratchFolder dir("clap-fx");
    const auto bundle = dir / "fx.sixteen";
    REQUIRE(services::store::NativeCodec::save(core::commands::project(bus), bundle).ok());
    auto reopened = newDefaultProject();
    services::store::ImportReport report;
    REQUIRE(services::store::NativeCodec::readInto(bundle, *reopened, report).ok());
    CHECK(core::fxSlotAt(*reopened, at) == slot());
    REQUIRE(bus.redo());
    CHECK(slot().plugin == b);
    bus.execute(std::make_unique<core::SelectFxType>(at, EffectType::Off));
    CHECK(slot().plugin == PluginTrack{});
}

TEST_CASE("A full engine bridge retries the latest project FX and a fresh project starts at factory state",
          "[plugin][fx]")
{
    auto engine = std::make_unique<engine::Engine>(enginetest::fullSetup());
    engine->prepare(44100, 256);
    auto& bridge = engine->bridge();
    services::plugin::ClapHostService host(bridge, CLAP_FIXTURE_DIR, 44100);
    auto project = core::model::newDefaultProject();
    auto& fx = project->scenes[0].sendFx;
    fx.type = core::model::EffectType::Plugin;
    fx.plugin = {.path = "clap_effect_fixture.clap", .pluginId = "test.stereo", .stateBase64 = "Mg=="};

    const engine::ParamChange fill{*core::builtinParams().idOf("track.mode"), 0, 0.0f, 0};
    while (bridge.post(fill))
    {
    }
    host.syncFromProject(*project);
    REQUIRE_FALSE(host.hasEffect(0, 32));
    SECTION("Recall was cancelled while the bridge was full")
    {
        fx.type = core::model::EffectType::Off;
        host.syncFromProject(*project);
        enginetest::render(*engine, 4096);
        host.pollMainThread();
        CHECK_FALSE(host.hasEffect(0, 32));
    }
    SECTION("Recall retries on the next poll and does not leak into a fresh project")
    {
        enginetest::render(*engine, 4096);
        host.pollMainThread();
        REQUIRE(host.hasEffect(0, 32));
        CHECK(host.mountedEffect(0, 32)->saveStateBase64() == "Mg==");
        fx.plugin.stateBase64.clear();
        host.syncFromProject(*project);
        REQUIRE(host.hasEffect(0, 32));
        CHECK(host.mountedEffect(0, 32)->saveStateBase64() == "GQ==");
    }
    enginetest::render(*engine, 256);
}

TEST_CASE("CLAP host shutdown releases queued and installed instruments and effects after the engine stops", "[plugin]")
{
    auto engine = std::make_unique<engine::Engine>(enginetest::fullSetup());
    engine->prepare(44100, 256);
    services::plugin::ClapHostService host(engine->bridge(), CLAP_FIXTURE_DIR, 44100);
    REQUIRE(host.mount(0, 0, {.path = "clap_effect_fixture.clap", .pluginId = "test.instrument"}));
    REQUIRE(host.mountEffect(0, 32, {.path = "clap_effect_fixture.clap", .pluginId = "test.stereo"}));

    SECTION("mounts still queued") {}
    SECTION("mounted nodes with a full command queue")
    {
        enginetest::render(*engine, 256);
        const engine::ParamChange fill{*core::builtinParams().idOf("track.mode"), 0, 0.0f, 0};
        while (engine->bridge().post(fill))
        {
        }
    }
    host.detachFromEngine();
    engine.reset();
    host.unmountAll();
    CHECK(host.mounted(0, 0) == nullptr);
    CHECK_FALSE(host.hasEffect(0, 32));
}

TEST_CASE("Track and FX plugin presets follow the data drive across projects", "[plugin][presets][store]")
{
    using namespace core::model;
    using ui::fx::Slot;
    storetest::ScratchFolder drive("plugin-presets");
    const auto plugins = drive / "Plugins";
    REQUIRE(plugins.createDirectory().wasOk());
    const juce::String binary = "clap_effect_fixture.clap";
    REQUIRE(juce::File(CLAP_FIXTURE_DIR).getChildFile(binary).copyFileTo(plugins.getChildFile(binary)));
    auto engine = std::make_unique<engine::Engine>(enginetest::fullSetup());
    engine->prepare(44100, 256);
    services::plugin::ClapHostService host(engine->bridge(), plugins.getFullPathName().toStdString(), 44100);
    core::CommandBus bus;
    app::PluginUiAdapter ui(bus, host);
    core::commands::setBridge(bus,
                              [&](const auto& changes, const auto& project) { host.syncChanges(changes, project); });

    auto newProject = [&](int scene, int track)
    {
        auto project = newDefaultProject();
        auto& tr = project->scenes[scene - 1].tracks[track - 1];
        tr.mode = TrackMode::Plugin;
        tr.plugin = {.path = binary.toStdString(), .pluginId = "test.instrument"};
        for (auto slot : {Slot::Fx1, Slot::Fx2, Slot::Send, Slot::Master})
        {
            auto& fx = core::fxSlotAt(*project, ui::fx::pathOf(slot, scene, track));
            fx.type = EffectType::Plugin;
            fx.plugin = {.path = binary.toStdString(), .pluginId = "test.stereo"};
            fx.params[0] = 0.625;
            fx.bypass = true;
        }
        core::commands::replaceProject(bus, std::move(project));
        host.syncFromProject(core::commands::project(bus));
    };
    auto saved = [](const core::Result<bool>& result)
    {
        if (! result.ok())
            FAIL(result.error().message);
        return result.value();
    };
    newProject(1, 1);
    // Save with both encoder events still queued, including on an instrument with no CLAP state extension.
    REQUIRE(ui.turnKnob(1, 1, 0, 10, false));
    REQUIRE(ui.turnKnob(1, 1, 0, 5, false));
    REQUIRE(saved(ui.savePreset(1, 1, {}, "Bright", false)));
    REQUIRE(ui.turnKnob(1, 1, 0, 25, false, Slot::Fx1));
    REQUIRE(saved(ui.savePreset(1, 1, Slot::Fx1, "Bright", false)));
    REQUIRE_FALSE(saved(ui.savePreset(1, 1, Slot::Fx1, "bright", false)));
    CHECK_FALSE(ui.savePreset(1, 1, {}, "../escape", false).ok());

    newProject(4, 6);
    auto userPreset = [&](std::optional<Slot> slot)
    {
        const auto list = ui.presets(4, 6, slot);
        REQUIRE(std::count_if(list.begin(), list.end(), [](const auto& e) { return e.user; }) == 1);
        const auto it =
            std::find_if(list.begin(), list.end(), [](const auto& e) { return e.user && e.name == "Bright"; });
        REQUIRE(it != list.end());
        return *it;
    };
    auto* track = host.mounted(3, 5);
    REQUIRE(track);
    REQUIRE(track->paramValue(42) == 25);
    REQUIRE(ui.loadPreset(4, 6, userPreset({})));
    REQUIRE(host.mounted(3, 5)->paramValue(42) == 40);
    for (auto slot : {Slot::Fx1, Slot::Fx2, Slot::Send, Slot::Master})
    {
        CAPTURE(static_cast<int>(slot));
        const auto index = ui::fx::EngineFxMeters::slotIndex(slot, 6);
        REQUIRE(host.mountedEffect(3, index)->paramValue(42) == 25);
        REQUIRE(ui.loadPreset(4, 6, userPreset(slot), slot));
        REQUIRE(host.mountedEffect(3, index)->paramValue(42) == 50);
        const auto at = ui::fx::pathOf(slot, 4, 6);
        const auto& fx = core::fxSlotAt(core::commands::project(bus), at);
        REQUIRE(fx.bypass);
        REQUIRE(fx.params[0] == 0.625);
        REQUIRE(bus.undo());
        REQUIRE(host.mountedEffect(3, index)->paramValue(42) == 25);
        REQUIRE(bus.redo());
        REQUIRE(host.mountedEffect(3, index)->paramValue(42) == 50);
    }

    const auto file = drive / "Second.sixteen";
    host.snapshotStates(core::commands::project(bus));
    REQUIRE(services::store::NativeCodec::save(core::commands::project(bus), file).ok());
    auto lib = (drive / "Presets").getChildFile("Plugins");
    services::plugin::UserPresets store(lib);
    REQUIRE(ui.turnKnob(4, 6, 0, 10, false, Slot::Master));
    REQUIRE(saved(ui.savePreset(4, 6, Slot::Master, "bright", true)));
    REQUIRE(store.names("test.stereo") == std::vector<std::string>{"Bright"});
    auto state = store.read("test.stereo", "BRIGHT");
    REQUIRE(state.ok());
    REQUIRE(state.value() == "PA=="); // 60, native state
    REQUIRE(saved(store.save("test.stereo", "Damaged", "!!!!", false)));
    const auto masterIndex = ui::fx::EngineFxMeters::slotIndex(Slot::Master, 6);
    auto* before = host.mountedEffect(3, masterIndex);
    REQUIRE_FALSE(ui.loadPreset(4, 6, {"Damaged", "Damaged", true}, Slot::Master));
    REQUIRE(host.mountedEffect(3, masterIndex) == before);
    REQUIRE(lib.deleteRecursively());
    auto reopened = newDefaultProject();
    services::store::ImportReport report;
    REQUIRE(services::store::NativeCodec::readInto(file, *reopened, report).ok());
    host.syncFromProject(*reopened);
    REQUIRE(host.mounted(3, 5)->paramValue(42) == 40);
    REQUIRE(host.mountedEffect(3, masterIndex)->paramValue(42) == 50);
    enginetest::render(*engine, 256);
}
