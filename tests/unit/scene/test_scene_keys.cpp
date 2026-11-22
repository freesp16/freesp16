// ui::input::scenes::ScenePatternKeys: PATTERN mode, two-stage SCENE mode (leaving without a pattern restores the
// scene), SHIFT + PATTERN length mode, step-key ownership, ASK policy, SwitchDone, and the lamps
// (UI-154/155/161, SCN-022/026/027).
#include "core/commands/CommandBusAccess.h"
#include "core/commands/scene/SceneCommands.h"
#include "core/model/ModelOps.h"
#include "core/params/ParamRegistry.h"
#include "tests/fakes/FakeEngineBridge.h"
#include "ui/input/scenes/ScenePatternKeys.h"
#include "ui/input/scenes/SceneSync.h"
#include "ui/theme/Theme.h"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <variant>

namespace sc = core::commands::scene;
namespace m = core::model;
using ui::input::scenes::KeyMode;

namespace
{
struct Rig
{
    core::CommandBus bus;
    fakes::FakeEngineBridge bridge;
    ui::input::scenes::SceneSync sync {core::builtinParams(), bridge};
    std::unique_ptr<ui::input::scenes::ScenePatternKeys> keys;
    engine::PlayheadFeed feed {};
    Rig()
    {
        core::commands::replaceProject(bus, m::newDefaultProject());
        core::commands::setBridge(bus, [this](const core::ChangeSet& c, const m::Project& p) { sync.onChange(c, p); });
        keys = std::make_unique<ui::input::scenes::ScenePatternKeys>(bus);
        feed.blockIndex = 1;
        keys->observe(feed);
    }
    m::Project& project() { return core::commands::project(bus); }
    std::vector<float> queued() const
    {
        const auto id = core::builtinParams().idOf("scene.queue").value();
        std::vector<float> out;
        for (const auto& msg : bridge.posted)
            if (const auto* c = std::get_if<engine::ParamChange>(&msg); c != nullptr && c->param == id)
                out.push_back(c->value);
        return out;
    }
    bool press(io::Control c, bool shift = false)
    {
        return keys->handle(io::ControlEvent {io::ControlEvent::Kind::Press, c, 127, 0, 0.0}, shift);
    }
    io::LampFrame lamps(double nowMs = 0.0)
    {
        io::LampFrame f;
        keys->lamps(f, feed, nowMs);
        return f;
    }
};

struct RecordingSink final : io::LampSink
{
    std::map<io::Control, std::pair<std::uint32_t, std::uint8_t>> lamps;
    void setLamp(io::Control c, std::uint32_t rgb, std::uint8_t level) override { lamps[c] = {rgb, level}; }
    void flush() override {}
};
} // namespace

TEST_CASE("scene.keys: PATTERN mode selects (queued) patterns of the current scene and owns the step keys",
          "[scene][keys]")
{
    Rig r;
    CHECK_FALSE(r.keys->ownsStepKeys());
    CHECK_FALSE(r.press(io::step(3))); // not ours while no mode is on: the sequencer's
    CHECK(r.press(io::Control::Pattern));
    CHECK(r.keys->mode() == KeyMode::Pattern);
    CHECK(r.keys->ownsStepKeys());
    CHECK(r.press(io::step(3)));
    CHECK(r.press(io::step(1))); // the current pattern again: the engine cancels (SCN-021)
    REQUIRE(r.queued().size() == 2);
    CHECK(r.queued()[0] == 2.0f);
    CHECK(r.queued()[1] == 0.0f);
    CHECK(r.press(io::Control::Pattern));
    CHECK(r.keys->mode() == KeyMode::Off);
    CHECK_FALSE(r.press(io::Control::Bar2)); // bar keys stay the sequencer's outside length mode
}

TEST_CASE("scene.keys: SCENE mode is two-stage; leaving without a pattern restores the previous scene",
          "[scene][keys]")
{
    Rig r;
    r.feed.scene = 1;
    r.feed.pattern = 4;
    r.keys->observe(r.feed);
    r.press(io::Control::Scene);
    CHECK(r.keys->mode() == KeyMode::Scene);
    r.press(io::step(6)); // scene 6: its patterns are shown, nothing sent yet
    CHECK(r.keys->mode() == KeyMode::ScenePatterns);
    CHECK(r.keys->shownScene() == 5);
    CHECK(r.queued().empty());
    r.press(io::Control::Scene); // leave without a pattern key
    CHECK(r.keys->mode() == KeyMode::Off);
    CHECK(r.queued().empty());
    CHECK(r.keys->shownScene() == 1); // still the previous scene

    r.press(io::Control::Scene);
    r.press(io::step(6));
    r.press(io::step(9)); // commit scene 6, pattern 9
    REQUIRE(r.queued().size() == 1);
    CHECK(r.queued()[0] == 5 * 16 + 8);
    CHECK(r.keys->mode() == KeyMode::ScenePatterns);
    // The engine switched: SwitchDone updates the model's current scene/pattern.
    engine::EngineEvent done {};
    done.kind = engine::EngineEvent::Kind::SwitchDone;
    done.value = 5 * 16 + 8;
    r.keys->onEngineEvent(done);
    CHECK(r.project().settings.currentScene == 5);
    CHECK(r.project().settings.currentPattern == 8);
}

TEST_CASE("scene.keys: SHIFT + PATTERN sets the length by bar keys and step keys (8–64)", "[scene][keys]")
{
    Rig r;
    CHECK(r.press(io::Control::Pattern, true));
    CHECK(r.keys->mode() == KeyMode::Length);
    CHECK(r.press(io::Control::Bar3));
    CHECK(r.project().scenes[0].patterns[0].length == 48);
    CHECK(r.press(io::step(5))); // in the last bar of 48: 32 + 5
    CHECK(r.project().scenes[0].patterns[0].length == 37);
    r.press(io::Control::Bar1);
    r.press(io::step(3)); // 3 steps: below the minimum
    CHECK(r.project().scenes[0].patterns[0].length == 8);
    r.press(io::Control::Bar4);
    CHECK(r.project().scenes[0].patterns[0].length == 64);
    CHECK(r.project().scenes[1].patterns[0].length == 16);
    const auto f = r.lamps();
    for (int k = 1; k <= 16; ++k)
        CHECK(f[io::step(k)].rgb == io::lampcolour::white);
    CHECK(f[io::Control::Bar4].mode == io::LampMode::On);
    CHECK(f[io::Control::Bar4].rgb == io::lampcolour::blue);
    CHECK(f[io::Control::Pattern].mode == io::LampMode::On);
    r.press(io::Control::Pattern); // PATTERN leaves
    CHECK(r.keys->mode() == KeyMode::Off);
    CHECK(r.bus.undo()); // the length edits within 1 s coalesce into one undo step (EDT-U2)
    CHECK(r.project().scenes[0].patterns[0].length == 16);
}

TEST_CASE("scene.keys: lamps — scene colour, content dim, empty off, current full, pending blinking 2 Hz",
          "[scene][keys]")
{
    Rig r;
    r.project().scenes[0].patterns[2].steps[0][0].trigger = 100; // pattern 3 has content
    r.project().scenes[4].patterns[0].steps[5][1].trigger = 100; // scene 5 has content
    r.bus.execute(sc::setSceneColour(0, 9));                     // invalidates the content cache
    r.feed.pendingScene = 0;
    r.feed.pendingPattern = 6;
    auto none = r.lamps();
    CHECK(none[io::step(1)].mode == io::LampMode::Off); // no mode: the frame is left alone
    r.press(io::Control::Pattern);
    auto f = r.lamps();
    const auto colour = ui::theme::sceneColour(10) & 0x00FFFFFFu;
    CHECK(f[io::step(1)].mode == io::LampMode::On);
    CHECK(f[io::step(1)].rgb == colour);
    CHECK(f[io::step(3)].mode == io::LampMode::Dim);
    CHECK(f[io::step(2)].mode == io::LampMode::Off);
    CHECK(f[io::step(7)].mode == io::LampMode::BlinkSlow);
    CHECK(f[io::Control::Pattern].mode == io::LampMode::On);
    CHECK(f[io::Control::Scene].mode == io::LampMode::Off);

    r.press(io::Control::Scene);
    f = r.lamps();
    CHECK(f[io::Control::Scene].mode == io::LampMode::On);
    CHECK(f[io::step(1)].mode == io::LampMode::On);  // current scene
    CHECK(f[io::step(5)].mode == io::LampMode::Dim); // scene 5 has a recorded pattern
    CHECK(f[io::step(5)].rgb == (ui::theme::sceneColour(5) & 0x00FFFFFFu));
    CHECK(f[io::step(2)].mode == io::LampMode::Off);

    RecordingSink sink;
    r.keys->paintLamps(sink, r.feed, 0.0);
    CHECK(sink.lamps.size() == 18); // SCENE, PATTERN, 16 step keys
    CHECK(sink.lamps[io::step(1)].second == 255);
    CHECK(sink.lamps[io::step(2)].second == 0);
}

TEST_CASE("scene.keys: ASK asks before switching to a scene with a BPM and sends the answer", "[scene][keys]")
{
    Rig r;
    r.bus.execute(sc::setSceneBpm(2, 90.0));
    r.keys->setBpmPolicy(ui::input::scenes::BpmPolicy::Ask);
    int asked = 0;
    std::function<void(bool)> answer;
    r.keys->setAskBpm(
        [&](int scene, double bpm, std::function<void(bool)> a)
        {
            ++asked;
            CHECK(scene == 2);
            CHECK(bpm == 90.0);
            answer = std::move(a);
        });
    r.press(io::Control::Scene);
    r.press(io::step(3));
    r.press(io::step(1));
    CHECK(asked == 1);
    CHECK(r.queued().empty()); // nothing queued before the answer
    answer(true);
    REQUIRE(r.queued().size() == 1);
    CHECK(r.queued()[0] == 2 * 16 + sc::kQueueApplyBpm);
    r.press(io::Control::Scene);
    r.press(io::Control::Scene);
    r.press(io::step(2)); // scene 2 has no BPM: no question
    r.press(io::step(1));
    CHECK(asked == 1);
    CHECK(r.queued().back() == 16.0f);
}
