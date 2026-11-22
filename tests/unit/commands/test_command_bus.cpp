// CommandBus (state-and-undo.md): execute → apply, record, notify, bridge; undo is an execute of the inverse through
// the same bridge path (EDT-U4); batched observer notification; recording-pass groups (EDT-U3); AssignSample undo
// restores the reference only (EDT-U5); checkpoints and dirty state (EDT-007); history cleared on open (EDT-U1).
#include "CommandTestFields.h"
#include "core/commands/CommandBusAccess.h"
#include "core/commands/CommandGroup.h"
#include "core/commands/UndoStack.h"
#include "tests/fakes/FakeClock.h"
#include "tests/fakes/FakeEngineBridge.h"

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <string>
#include <vector>

namespace
{
using namespace cmdtest;
namespace cmd = core::commands;

// A transport-like command: applied and bridged, never recorded.
class SelectScene final : public core::Command
{
public:
    explicit SelectScene(int s) : scene_(s) {}
    std::string_view label() const override { return "SCENE"; }
    void apply(Project& p) override { p.settings.currentScene = scene_; }
    std::unique_ptr<core::Command> inverse(const Project&) const override { return nullptr; }
    bool undoable() const override { return false; }
    void collectChanges(core::ChangeSet& c) const override { c.add("settings.currentScene"); }

private:
    int scene_;
};
} // namespace

TEST_CASE("command bus: execute applies, undo and redo restore", "[commands]")
{
    core::CommandBus bus;
    giveProject(bus);
    const Project& p = cmd::project(bus);
    CHECK_FALSE(bus.undo());
    CHECK_FALSE(bus.redo());
    bus.execute(nullptr); // ignored
    bus.execute(setVolume(2, 5, 77));
    CHECK(p.scenes[2].tracks[5].volume == 77);
    CHECK(cmd::history(bus).undoLabel() == "VOLUME");
    REQUIRE(bus.undo());
    CHECK(p.scenes[2].tracks[5].volume == 100);
    REQUIRE(bus.redo());
    CHECK(p.scenes[2].tracks[5].volume == 77);

    // Non-undoable commands are applied but not recorded and do not touch the history.
    bus.execute(std::make_unique<SelectScene>(4));
    CHECK(p.settings.currentScene == 4);
    CHECK(cmd::history(bus).undoDepth() == 1);
    REQUIRE(bus.undo());
    CHECK(p.settings.currentScene == 4);
    CHECK(p.scenes[2].tracks[5].volume == 100);
}

TEST_CASE("command bus: every execute, undo and redo reaches the bridge (EDT-U4)", "[commands]")
{
    core::CommandBus bus;
    giveProject(bus);
    fakes::FakeEngineBridge engine;
    std::vector<std::string> bridged;
    cmd::setBridge(bus,
                   [&](const core::ChangeSet& c, const Project& p)
                   {
                       engine.apply(c, p);
                       for (const auto& path : c.paths())
                           bridged.push_back(path + "=" + std::to_string(p.scenes[0].tracks[1].volume));
                   });
    bus.execute(setVolume(0, 1, 64));
    bus.undo();
    bus.redo();
    CHECK(engine.applied == 3);
    CHECK(bridged == std::vector<std::string>{"scene[0].track[1].volume=64", "scene[0].track[1].volume=100",
                                              "scene[0].track[1].volume=64"});
}

TEST_CASE("command bus: notifications are batched per message-loop iteration", "[commands]")
{
    core::CommandBus bus;
    giveProject(bus);
    int patternCalls = 0, trackCalls = 0, allCalls = 0;
    std::size_t lastSize = 0;
    auto s1 = bus.observe("scene[0].pattern[2]",
                          [&](const core::ChangeSet& c)
                          {
                              ++patternCalls;
                              lastSize = c.size();
                          });
    auto s2 = bus.observe("scene[0].track[", [&](const core::ChangeSet&) { ++trackCalls; });
    auto s3 = bus.observe("", [&](const core::ChangeSet&) { ++allCalls; });

    // A 64-step paste as one group: one undo step, one notification.
    auto paste = std::make_unique<core::CommandGroup>("PASTE");
    for (int st = 0; st < 64; ++st)
        paste->add(setStep(0, 2, st, 7, core::model::Step{static_cast<std::int16_t>(st), 100}));
    bus.execute(std::move(paste));
    CHECK(patternCalls == 0); // nothing until the loop iteration ends
    CHECK(cmd::notificationsPending(bus));
    cmd::flushNotifications(bus);
    CHECK(patternCalls == 1);
    CHECK(lastSize == 64);
    CHECK(trackCalls == 0);
    CHECK(allCalls == 1);
    CHECK(cmd::history(bus).undoDepth() == 1);

    // Many edits of one path in one iteration: one call, the path once.
    for (int v = 0; v < 20; ++v)
        bus.execute(setVolume(0, 3, v));
    cmd::flushNotifications(bus);
    CHECK(trackCalls == 1);
    CHECK(allCalls == 2);
    cmd::flushNotifications(bus); // nothing pending: nobody is called
    CHECK(allCalls == 2);

    // Undo is notified like any edit.
    bus.undo();
    cmd::flushNotifications(bus);
    CHECK(trackCalls == 2);

    // Unsubscribing stops delivery.
    s2.reset();
    bus.execute(setVolume(0, 3, 1));
    cmd::flushNotifications(bus);
    CHECK(trackCalls == 2);
    CHECK(allCalls == 4);
}

TEST_CASE("command bus: a scheduler flushes once per batch; callbacks survive the bus", "[commands]")
{
    std::vector<std::function<void()>> loop; // a stand-in for the message loop's queue
    std::function<void()> late;
    int calls = 0;
    {
        core::CommandBus bus;
        giveProject(bus);
        cmd::setNotifyScheduler(bus, [&](std::function<void()> f) { loop.push_back(std::move(f)); });
        auto sub = bus.observe("scene[1]", [&](const core::ChangeSet&) { ++calls; });
        bus.execute(setVolume(1, 0, 1));
        bus.execute(setVolume(1, 1, 2));
        bus.execute(setPan(1, 1, 3));
        REQUIRE(loop.size() == 1);
        loop.front()();
        loop.clear();
        CHECK(calls == 1);
        bus.execute(setVolume(1, 0, 4));
        REQUIRE(loop.size() == 1);
        late = loop.front();
        // An observer may unsubscribe itself and others, and subscribe new ones, while being notified.
        core::Subscription extra;
        auto self = std::make_shared<core::Subscription>();
        *self = bus.observe("scene[1]",
                            [&](const core::ChangeSet&)
                            {
                                self->reset();
                                extra = bus.observe("scene[1]", [&](const core::ChangeSet&) { calls += 100; });
                            });
        late();
        CHECK(calls == 2);
        CHECK_FALSE(self->active());
        bus.execute(setVolume(1, 0, 5));
        cmd::flushNotifications(bus);
        CHECK(calls == 103);
    }
    late(); // the bus is gone: does nothing
    CHECK(calls == 103);
}

TEST_CASE("command bus: Subscription outlives the bus safely", "[commands]")
{
    core::Subscription sub;
    {
        core::CommandBus bus;
        giveProject(bus);
        sub = bus.observe("x", [](const core::ChangeSet&) {});
    }
    sub.reset(); // no crash, no access to the destroyed bus
    CHECK_FALSE(sub.active());
}

TEST_CASE("command bus: coalescing uses the injected clock (EDT-U2)", "[commands][undo]")
{
    core::CommandBus bus;
    giveProject(bus);
    fakes::FakeClock clock;
    cmd::setClock(bus, &clock);
    for (int i = 0; i < 30; ++i)
    {
        bus.execute(setVolume(0, 0, 100 - i));
        clock.advance(200);
    }
    CHECK(cmd::history(bus).undoDepth() == 1);
    clock.advance(1500);
    bus.execute(setVolume(0, 0, 10));
    CHECK(cmd::history(bus).undoDepth() == 2);
    bus.undo();
    CHECK(cmd::project(bus).scenes[0].tracks[0].volume == 71);
    bus.undo();
    CHECK(cmd::project(bus).scenes[0].tracks[0].volume == 100);
    cmd::setClock(bus, nullptr);
}

TEST_CASE("command bus: a recording pass is one undo step (EDT-U3)", "[commands][undo]")
{
    core::CommandBus bus;
    giveProject(bus);
    fakes::FakeClock clock;
    cmd::setClock(bus, &clock);
    int bridgeCalls = 0;
    cmd::setBridge(bus, [&](const core::ChangeSet&, const Project&) { ++bridgeCalls; });
    const Project& p = cmd::project(bus);
    bus.execute(setTempo(100));
    {
        cmd::ScopedGroup pass(bus);
        CHECK(cmd::groupOpen(bus));
        for (int st = 0; st < 16; st += 4)
        {
            bus.execute(setStep(0, 0, st, 0, core::model::Step{1, 120}));
            clock.advance(5000); // far apart: still one step
        }
        for (int v = 0; v < 10; ++v)
            bus.execute(setVolume(0, 0, v));                        // a knob turned during the pass (p-lock style)
        bus.execute(setStep(0, 0, 8, 0, core::model::Step{2, 60})); // the same cell twice: undone newest first
        bus.execute(setStep(0, 0, 8, 0, core::model::Step{3, 70}));
        CHECK(bus.dirty());
        CHECK(p.scenes[0].patterns[0].steps[4][0].trigger == 1); // audible during the pass
    }
    CHECK_FALSE(cmd::groupOpen(bus));
    CHECK(bridgeCalls == 17);
    CHECK(cmd::history(bus).undoDepth() == 2);
    CHECK(cmd::history(bus).undoLabel() == "RECORD");
    REQUIRE(bus.undo());
    CHECK(p.scenes[0].patterns[0].steps[4][0].empty());
    CHECK(p.scenes[0].patterns[0].steps[8][0].empty());
    CHECK(p.scenes[0].tracks[0].volume == 100);
    CHECK(p.settings.tempo == 100);
    REQUIRE(bus.redo());
    CHECK(p.scenes[0].patterns[0].steps[12][0].trigger == 1);
    CHECK(p.scenes[0].patterns[0].steps[8][0].trigger == 3);
    CHECK(p.scenes[0].tracks[0].volume == 9);

    // An empty pass records nothing; undo during a pass ends it first and undoes it.
    cmd::beginGroup(bus);
    cmd::endGroup(bus);
    CHECK(cmd::history(bus).undoDepth() == 2);
    cmd::beginGroup(bus, "RECORD");
    bus.execute(setStep(0, 0, 1, 1, core::model::Step{1, 90}));
    REQUIRE(bus.undo());
    CHECK_FALSE(cmd::groupOpen(bus));
    CHECK(p.scenes[0].patterns[0].steps[1][1].empty());
    CHECK(cmd::history(bus).undoDepth() == 2);
}

TEST_CASE("command bus: AssignSample undo restores the reference only (EDT-U5)", "[commands][undo]")
{
    core::CommandBus bus;
    giveProject(bus);
    const Project& p = cmd::project(bus);
    bus.execute(std::make_unique<core::AssignSample>(0, 2, sampleRef("kick")));
    REQUIRE(p.sampleRefs.entries.size() == 1);
    CHECK(p.scenes[0].tracks[2].playback.sample.id == "kick");
    bus.execute(std::make_unique<core::AssignSample>(0, 3, sampleRef("kick"))); // shared: pool unchanged
    bus.execute(std::make_unique<core::AssignSample>(0, 2, sampleRef("snare")));
    CHECK(p.sampleRefs.entries.size() == 2);
    CHECK(p.scenes[0].tracks[2].playback.sample.id == "snare");

    REQUIRE(bus.undo());
    CHECK(p.scenes[0].tracks[2].playback.sample.id == "kick"); // the previous reference is back
    REQUIRE(p.sampleRefs.entries.size() == 1);                 // the record this assignment added is gone …
    CHECK(p.sampleRefs.find({"kick"}) != nullptr);             // … the one still referenced stays
    REQUIRE(bus.undo());
    CHECK(p.scenes[0].tracks[3].playback.sample.empty());
    REQUIRE(bus.undo());
    CHECK(p.scenes[0].tracks[2].playback.sample.empty());
    CHECK(p.sampleRefs.entries.empty());
    REQUIRE(bus.redo());
    REQUIRE(bus.redo());
    REQUIRE(bus.redo());
    REQUIRE(p.sampleRefs.entries.size() == 2);
    CHECK(p.sampleRefs.entries[1] == sampleRef("snare")); // the full record (path, name) comes back on redo
    CHECK(p.scenes[0].tracks[2].playback.sample.id == "snare");

    // Clearing an assignment is undoable too.
    bus.execute(std::make_unique<core::AssignSample>(0, 2, core::model::SampleRef{}));
    CHECK(p.scenes[0].tracks[2].playback.sample.empty());
    bus.undo();
    CHECK(p.scenes[0].tracks[2].playback.sample.id == "snare");
}

TEST_CASE("command bus: checkpoint, dirty and clearHistory (EDT-007, EDT-U1)", "[commands][undo]")
{
    core::CommandBus bus;
    giveProject(bus);
    CHECK_FALSE(bus.dirty());
    bus.execute(setVolume(0, 0, 1));
    CHECK(bus.dirty());
    bus.checkpoint();
    CHECK_FALSE(bus.dirty());
    bus.execute(std::make_unique<SelectScene>(2)); // selection does not dirty the document (our choice)
    CHECK_FALSE(bus.dirty());
    bus.undo();
    CHECK(bus.dirty());
    bus.redo();
    CHECK_FALSE(bus.dirty());

    // Saving during a recording pass closes the pass first, so the saved state is an undo state.
    cmd::beginGroup(bus);
    bus.execute(setVolume(0, 0, 2));
    CHECK(bus.dirty());
    bus.checkpoint();
    CHECK_FALSE(bus.dirty());
    CHECK_FALSE(cmd::groupOpen(bus));

    bus.clearHistory(); // project open
    CHECK_FALSE(bus.dirty());
    CHECK_FALSE(cmd::history(bus).canUndo());
    CHECK_FALSE(bus.undo());
    CHECK(cmd::project(bus).scenes[0].tracks[0].volume == 2);

    auto next = std::make_unique<Project>();
    next->settings.tempo = 140;
    bus.execute(setTempo(90));
    auto previous = cmd::replaceProject(bus, std::move(next));
    CHECK(previous->settings.tempo == 90);
    CHECK(cmd::project(bus).settings.tempo == 140);
    CHECK_FALSE(bus.dirty());
    CHECK_FALSE(cmd::history(bus).canUndo());
    CHECK_FALSE(cmd::notificationsPending(bus));
}

TEST_CASE("command bus: the history cap evicts through the bus (EDT-U1)", "[commands][undo]")
{
    core::CommandBus bus;
    giveProject(bus);
    fakes::FakeClock clock;
    cmd::setClock(bus, &clock);
    CHECK(cmd::history(bus).capBytes() == core::UndoStack::kDefaultCapBytes);
    cmd::setHistoryCapBytes(bus, 4096);
    for (int i = 0; i < 1000; ++i)
    {
        bus.execute(setPan(i % 16, i % 16, i % 64));
        clock.advance(2000);
    }
    CHECK(cmd::history(bus).bytes() <= 4096);
    CHECK(cmd::history(bus).undoDepth() < 1000);
    CHECK(cmd::history(bus).evicted() > 0);
    std::size_t undone = 0;
    while (bus.undo())
        ++undone;
    CHECK(undone > 0);
    CHECK(cmd::history(bus).bytes() <= 4096);
}

TEST_CASE("command group: nested groups and the plain inverse", "[commands][undo]")
{
    auto p = std::make_unique<Project>();
    auto inner = std::make_unique<core::CommandGroup>("INNER");
    inner->add(setVolume(0, 0, 10));
    inner->add(setVolume(0, 0, 20)); // the same field twice: a sequential inverse is required
    core::CommandGroup outer("OUTER");
    outer.add(std::move(inner));
    outer.add(setPan(0, 0, -5));
    outer.add(std::make_unique<SelectScene>(3)); // not undoable: applied, no inverse
    CHECK(outer.undoable());
    core::ChangeSet c;
    outer.collectChanges(c);
    CHECK(c.size() == 4);
    CHECK(outer.sizeBytes() > 0);

    auto inv = core::applyAndInvert(outer, *p);
    CHECK(p->scenes[0].tracks[0].volume == 20);
    CHECK(p->settings.currentScene == 3);
    REQUIRE(inv);
    auto redo = core::applyAndInvert(*inv, *p);
    CHECK(p->scenes[0].tracks[0].volume == 100);
    CHECK(p->scenes[0].tracks[0].pan == 0);
    core::applyAndInvert(*redo, *p);
    CHECK(p->scenes[0].tracks[0].volume == 20);

    // Command::inverse(before) of a group of value commands, outside the bus.
    auto fresh = std::make_unique<Project>();
    auto plainInv = outer.inverse(*fresh);
    outer.apply(*fresh);
    plainInv->apply(*fresh);
    CHECK(fresh->scenes[0].tracks[0].volume == 100);
    CHECK(fresh->scenes[0].tracks[0].pan == 0);

    core::CommandGroup none("NONE");
    none.add(std::make_unique<SelectScene>(1));
    CHECK_FALSE(none.undoable());
    CHECK(core::applyAndInvert(none, *p) == nullptr);
}

TEST_CASE("command bus: without a project every edit is ignored", "[commands]")
{
    core::CommandBus bus;
    CHECK_FALSE(cmd::hasProject(bus));
    int bridged = 0;
    cmd::setBridge(bus, [&](const core::ChangeSet&, const Project&) { ++bridged; });
    bus.execute(setVolume(0, 0, 1));
    CHECK_FALSE(bus.undo());
    CHECK_FALSE(bus.redo());
    CHECK(bridged == 0);
    CHECK_FALSE(cmd::notificationsPending(bus));
    CHECK_FALSE(bus.dirty());
    giveProject(bus);
    REQUIRE(cmd::hasProject(bus));
    bus.execute(setVolume(0, 0, 1));
    CHECK(bridged == 1);
    CHECK(cmd::project(bus).scenes[0].tracks[0].volume == 1);
    CHECK(cmd::replaceProject(bus, nullptr) != nullptr);
    CHECK_FALSE(cmd::hasProject(bus));
}
