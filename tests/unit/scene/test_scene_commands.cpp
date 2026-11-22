// core::commands::scene and ui::input::scenes::SceneSync: undoable scene/pattern edits (bit-exact undo, scene
// isolation SCN-002), not-undoable switch requests (never dirty) and their translation to engine ParamChanges; the
// scene.cpp catalogue.
#include "core/commands/CommandBusAccess.h"
#include "core/commands/scene/SceneCommands.h"
#include "core/model/ModelOps.h"
#include "core/params/ParamRegistry.h"
#include "tests/fakes/FakeEngineBridge.h"
#include "ui/input/scenes/SceneSync.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <variant>

namespace sc = core::commands::scene;
namespace m = core::model;

namespace
{
struct Bus
{
    core::CommandBus bus;
    fakes::FakeEngineBridge bridge;
    ui::input::scenes::SceneSync sync {core::builtinParams(), bridge};
    Bus()
    {
        core::commands::replaceProject(bus, m::newDefaultProject());
        core::commands::setBridge(bus, [this](const core::ChangeSet& c, const m::Project& p) { sync.onChange(c, p); });
    }
    m::Project& project() { return core::commands::project(bus); }
    // The ParamChanges posted for `param` (in order).
    std::vector<engine::ParamChange> posted(const char* param) const
    {
        const auto id = core::builtinParams().idOf(param).value();
        std::vector<engine::ParamChange> out;
        for (const auto& msg : bridge.posted)
            if (const auto* c = std::get_if<engine::ParamChange>(&msg); c != nullptr && c->param == id)
                out.push_back(*c);
        return out;
    }
};
} // namespace

TEST_CASE("scene.commands: scene 3 edits never touch scene 4; undo restores bit-identically", "[scene][commands]")
{
    Bus b;
    const auto before = std::make_unique<m::Project>(b.project());
    b.bus.execute(sc::setSceneName(2, "Verse"));
    b.bus.execute(sc::setSceneColour(2, 7));
    b.bus.execute(sc::setSceneBpm(2, 90.0));
    b.bus.execute(sc::setPatternLength(2, 5, 40));
    auto pattern = std::make_unique<m::Pattern>(m::defaultPattern());
    pattern->steps[3][6].trigger = 100;
    pattern->trackMutes[4] = true;
    b.bus.execute(sc::replacePattern(2, 6, *pattern));
    const auto& p = b.project();
    CHECK(p.scenes[2].name == "Verse");
    CHECK(p.scenes[2].colour == 7);
    CHECK(p.scenes[2].bpm == 90.0);
    CHECK(p.scenes[2].patterns[5].length == 40);
    CHECK(p.scenes[2].patterns[6] == *pattern);
    CHECK(p.scenes[3] == before->scenes[3]); // scene 4 unchanged (SCN-002)
    CHECK(p.scenes[2].tracks == before->scenes[2].tracks);
    CHECK(b.bus.dirty());
    while (b.bus.undo())
    {
    }
    CHECK(b.project() == *before);
    CHECK(b.bus.redo());
    CHECK(b.project().scenes[2].name == "Verse");
}

TEST_CASE("scene.commands: replace / clear a scene or a pattern; length clamps; BPM 0 = none", "[scene][commands]")
{
    Bus b;
    const auto before = std::make_unique<m::Project>(b.project());
    b.bus.execute(sc::setSceneName(0, "Intro"));
    b.bus.execute(sc::setSceneBpm(0, 100.0));
    b.bus.execute(sc::replaceScene(5, b.project().scenes[0]));
    CHECK(b.project().scenes[5].name == "Intro");
    CHECK(b.project().scenes[5].bpm == 100.0);
    b.bus.execute(sc::clearScene(5));
    auto fresh = std::make_unique<m::Scene>(before->scenes[5]);
    CHECK(b.project().scenes[5] == *fresh); // the defaults of scene 6 (its colour)
    b.bus.undo();
    CHECK(b.project().scenes[5].name == "Intro");
    b.bus.execute(sc::setPatternLength(0, 0, 200));
    CHECK(b.project().scenes[0].patterns[0].length == 64);
    b.bus.execute(sc::setPatternLength(0, 0, 0));
    CHECK(b.project().scenes[0].patterns[0].length == 1);
    b.project().scenes[0].patterns[3].steps[0][0].trigger = 64;
    b.bus.execute(sc::clearPattern(0, 3));
    CHECK(b.project().scenes[0].patterns[3] == m::defaultPattern());
    b.bus.execute(sc::setSceneBpm(0, 0.0));
    CHECK_FALSE(b.project().scenes[0].bpm.has_value());
}

TEST_CASE("scene.commands: switch requests are not undoable, never dirty, and post scene.queue", "[scene][commands]")
{
    Bus b;
    b.bus.checkpoint();
    b.bus.execute(sc::selectScenePattern(2, 5));
    b.bus.execute(sc::selectScenePattern(1, 0, true));
    b.bus.execute(sc::cancelScenePattern());
    b.bus.execute(sc::setSwitchable(false));
    b.bus.execute(sc::setSwitchable(true));
    CHECK_FALSE(b.bus.dirty());
    CHECK_FALSE(b.bus.undo());
    const auto q = b.posted("scene.queue");
    REQUIRE(q.size() == 5);
    CHECK(q[0].value == 2 * 16 + 5);
    CHECK(q[0].scope == 0xFFFF);
    CHECK(q[1].value == 16 + sc::kQueueApplyBpm);
    CHECK(q[2].value == sc::kQueueCancel);
    CHECK(q[3].value == sc::kQueueBlock);
    CHECK(q[4].value == sc::kQueueUnblock);
    // The engine's SwitchDone → the model's current scene/pattern (saved with the project, still not dirty).
    b.bus.execute(sc::setCurrentScenePattern(2, 5));
    CHECK(b.project().settings.currentScene == 2);
    CHECK(b.project().settings.currentPattern == 5);
    CHECK(b.posted("scene.queue").size() == 5); // not a request
    CHECK_FALSE(b.bus.dirty());

    CHECK(sc::queuePath(37) == "queue[37]");
    CHECK(sc::queueValueOf("queue[-1]") == -1);
    CHECK_FALSE(sc::queueValueOf("queue[x]").has_value());
    CHECK_FALSE(sc::queueValueOf("scene[1].bpm").has_value());
}

TEST_CASE("scene.sync: model edits reach the engine as scene / pattern ParamChanges with their scopes",
          "[scene][commands]")
{
    Bus b;
    b.bus.execute(sc::setSceneBpm(3, 95.0));
    b.bus.execute(sc::setPatternLength(3, 7, 24));
    auto pattern = std::make_unique<m::Pattern>(m::defaultPattern());
    pattern->trackMutes[0] = pattern->trackMutes[15] = true;
    pattern->trackSolos[2] = true;
    b.bus.execute(sc::replacePattern(3, 7, *pattern));
    b.bus.undo();     // back to the length-24 pattern
    b.bus.execute(sc::setSceneBpm(3, std::nullopt));
    const auto bpm = b.posted("scene.bpm");
    REQUIRE(bpm.size() == 2);
    CHECK(bpm[0].scope == 3);
    CHECK(bpm[0].value == 95.0f);
    CHECK(bpm[1].value == 0.0f);
    const auto length = b.posted("pattern.length");
    REQUIRE(length.size() == 3);
    CHECK(length[0].scope == (3 << 4 | 7));
    CHECK(length[0].value == 24.0f);
    CHECK(length[1].value == 16.0f); // the replaced pattern
    CHECK(length[2].value == 24.0f); // undo (EDT-U4: the engine hears the undo)
    const auto mutes = b.posted("pattern.mutes"), solos = b.posted("pattern.solos");
    REQUIRE(mutes.size() == 3);
    CHECK(mutes[1].value == static_cast<float>(0x8001));
    CHECK(solos[1].value == static_cast<float>(0x0004));
    CHECK(mutes[2].value == 0.0f);

    // A whole-scene paste posts its BPM and its 16 patterns; the quantize unit; syncAll covers the project.
    const auto n = b.bridge.posted.size();
    b.bus.execute(sc::replaceScene(4, b.project().scenes[3]));
    CHECK(b.bridge.posted.size() - n == 1 + 16 * 3);
    b.project().settings.patternQuantize = m::QuantizeUnit::Bar;
    core::ChangeSet cs;
    cs.add("settings.patternQuantize");
    b.sync.onChange(cs, b.project());
    REQUIRE(b.posted("project.patternQuantize").size() == 1);
    CHECK(b.posted("project.patternQuantize")[0].value == 1.0f);
    b.bridge.posted.clear();
    b.project().settings.currentScene = 6;
    b.project().settings.currentPattern = 2;
    b.sync.syncAll(b.project());
    CHECK(b.bridge.posted.size() == 16 * (1 + 16 * 3) + 2);
    CHECK(b.posted("scene.queue").back().value == 6 * 16 + 2);
}

TEST_CASE("scene.params: the scene.cpp ids, ranges, defaults and scopes", "[scene][params]")
{
    const auto& r = core::builtinParams();
    struct Expect
    {
        const char* id;
        float min, max, def;
        core::ParamScope scope;
    };
    using S = core::ParamScope;
    for (const Expect& e : {Expect {"scene.name", -1e6f, 1e6f, 0, S::Scene}, Expect {"scene.color", 0, 15, 0, S::Scene},
                            Expect {"scene.bpm", 0, 300, 0, S::SceneOverrideElseProject},
                            Expect {"scene.queue", -3, 511, -1, S::Project},
                            Expect {"pattern.length", 8, 64, 16, S::Scene},
                            Expect {"pattern.mutes", 0, 65535, 0, S::Scene},
                            Expect {"pattern.solos", 0, 65535, 0, S::Scene},
                            Expect {"project.patternQuantize", 0, 2, 2, S::Project},
                            Expect {"scene.bpmPolicy", 0, 2, 1, S::Global}, // ASK (utility.md)
                            Expect {"scene.switchMode", 0, 1, 0, S::Global}})
    {
        INFO(e.id);
        const auto* s = r.find(e.id);
        REQUIRE(s != nullptr);
        CHECK(s->minValue == e.min);
        CHECK(s->maxValue == e.max);
        CHECK(s->defaultValue == e.def);
        CHECK(s->scope == e.scope);
        CHECK(s->featureFlag.empty()); // real specs, not the interim stubs
        CHECK_FALSE(s->lockable);
        CHECK(s->shortLabel.size() <= 8);
    }
    char buf[32];
    const auto* bpm = r.find("scene.bpm");
    CHECK(std::string_view(buf, static_cast<std::size_t>(bpm->formatValue(0.0f, buf, 32))) == "---");
    CHECK(std::string_view(buf, static_cast<std::size_t>(bpm->formatValue(90.0f, buf, 32))) == "90.0");
    sc::setFocusedSceneName("Chorus");
    const auto* name = r.find("scene.name");
    CHECK(std::string_view(buf, static_cast<std::size_t>(name->formatValue(3.0f, buf, 32))) == "Chorus");
    sc::setFocusedSceneName("");
    CHECK(std::string_view(buf, static_cast<std::size_t>(name->formatValue(3.0f, buf, 32))) == "---");
    const auto* q = r.find("project.patternQuantize");
    CHECK(std::string_view(buf, static_cast<std::size_t>(q->formatValue(2.0f, buf, 32))) == "PATTERN");
}

TEST_CASE("scene.commands: a longer pattern repeats its bars into the new steps (hardware feedback)", "[scene][commands]")
{
    Bus b;
    auto& p = b.project();
    auto& pat = p.scenes[0].patterns[0];
    REQUIRE(pat.length == 16);
    pat.steps[0][0].trigger = 100;
    pat.steps[4][2].trigger = 90;
    m::StepLocks lock;
    lock.mask = 1;
    lock.values[0] = 0.5f;
    pat.locks.push_back(lock);
    pat.steps[4][2].lockIndex = 0;
    const auto before = std::make_unique<m::Pattern>(pat);

    auto grow = sc::resizePattern(p, 0, 0, 64); // 1 → 4 bars: bar 1 four times
    b.bus.execute(std::move(grow));
    CHECK(pat.length == 64);
    for (int bar = 0; bar < 4; ++bar)
    {
        CHECK(pat.steps[static_cast<std::size_t>(bar * 16)][0].trigger == 100);
        CHECK(pat.steps[static_cast<std::size_t>(bar * 16 + 4)][2].trigger == 90);
        const auto li = pat.steps[static_cast<std::size_t>(bar * 16 + 4)][2].lockIndex;
        REQUIRE(li < pat.locks.size());
        CHECK(pat.locks[li].values[0] == 0.5f);
        if (bar > 0)
            CHECK(li != 0); // each copy has its own locks
    }
    CHECK(pat.steps[1][0].empty());
    b.bus.undo(); // one undo step restores the pattern bit for bit
    CHECK(pat == *before);

    // One step at a time (the SEQUENCE screen's LENGTH) still copies bar 1 bar-wise.
    for (int n = 17; n <= 32; ++n)
        b.bus.execute(sc::resizePattern(p, 0, 0, n));
    CHECK(pat.steps[16][0].trigger == 100);
    CHECK(pat.steps[20][2].trigger == 90);
    CHECK(pat.steps[17][0].empty());

    // Shorter, then longer again: the steps kept beyond the length come back as they were (no copy over them).
    pat.steps[20][2].trigger = 33;
    b.bus.execute(sc::resizePattern(p, 0, 0, 16));
    b.bus.execute(sc::resizePattern(p, 0, 0, 32));
    CHECK(pat.steps[20][2].trigger == 33);

    // An empty pattern only changes its length.
    auto& empty = p.scenes[0].patterns[1];
    b.bus.execute(sc::resizePattern(p, 0, 1, 48));
    CHECK(empty.length == 48);
    CHECK(empty.steps[16][0].empty());
}
