// The step keys on the message thread (ui/input/steps): step entry (SEQ-040), hold + turn locks (SEQ-022), the
// recording pass as one undo step from the engine's events (EDT-U3, SEQ-041/046), encoder recording (SEQ-023), step
// lamps (SEQ-013), the StepLockAccess decorator, the scene keys' priority, and the pattern sync that
// turns ChangeSets into ObjectSwaps of PatternData (and an end-to-end edit heard by the engine).
#include "tests/fakes/FakeEngineBridge.h"
#include "tests/unit/sequencer/SeqRig.h"

#include "core/commands/CommandBusAccess.h"
#include "core/commands/UndoStack.h"
#include "core/model/ModelOps.h"
#include "core/model/Project.h"
#include "engine/sequencer/PatternBuilder.h"
#include "ui/input/steps/PatternSync.h"
#include "ui/input/steps/StepController.h"
#include "io/controllers/LampRules.h"

#include <catch2/catch_test_macros.hpp>

#include <map>

namespace steps = ui::input::steps;
using core::model::TriggerType;

namespace
{
struct Lamps final : io::LampSink
{
    std::map<io::Control, std::pair<std::uint32_t, std::uint8_t>> lit;
    void setLamp(io::Control c, std::uint32_t rgb, std::uint8_t level) override { lit[c] = {rgb, level}; }
    void flush() override {}
};

struct Fixture
{
    core::CommandBus bus;
    fakes::FakeEngineBridge bridge;
    steps::StepController sc {bus, &bridge};
    Fixture()
    {
        core::commands::replaceProject(bus, core::model::newDefaultProject());
        steps::setShownBar(0);
        sc.setContext(0, 0, 2);
    }
    ~Fixture()
    {
        for (const auto& m : bridge.posted) // the fake keeps what the sync posted: free the pattern objects
            if (const auto* s = std::get_if<engine::ObjectSwap>(&m); s != nullptr && s->slot.kind == engine::SwapSlot::Pattern)
                engine::sequencer::destroyPattern(s->object);
    }
    core::model::Pattern& pat(int s = 0, int q = 0)
    {
        return core::commands::project(bus).scenes[static_cast<std::size_t>(s)].patterns[static_cast<std::size_t>(q)];
    }
    const core::model::Step& cell(int step, int track) { return pat().steps[static_cast<std::size_t>(step)][static_cast<std::size_t>(track)]; }
    float posted(const char* id, std::uint16_t scope = 0xFFFF) const
    {
        const auto want = seqtest::id(id);
        float v = -1;
        for (const auto& m : bridge.posted)
            if (const auto* c = std::get_if<engine::ParamChange>(&m); c != nullptr && c->param == want &&
                                                                       (scope == 0xFFFF || c->scope == scope))
                v = c->value;
        return v;
    }
};
const core::ParamSpec& spec(const char* id) { return *core::builtinParams().find(id); }
} // namespace

TEST_CASE("seq.steps: a short step-key press toggles a trigger of the selected track on the shown bar", "[seq][steps]")
{
    Fixture f;
    f.sc.onStepKey(5, true, 0);
    f.sc.onStepKey(5, false, 80);
    CHECK(f.cell(4, 2).trigger == 15);
    CHECK(f.cell(4, 2).value2 == 127);
    steps::setShownBar(2);
    f.sc.setEntryType(TriggerType::Half);
    f.sc.onStepKey(1, true, 100);
    f.sc.onStepKey(1, false, 150);
    CHECK(f.cell(32, 2).triggerType == TriggerType::Half);
    f.sc.onStepKey(1, true, 200);
    f.sc.onStepKey(1, false, 250);
    CHECK(f.cell(32, 2).empty());
    // The scene keys own the step keys (SCENE / PATTERN mode): nothing happens.
    f.sc.setStepKeysOwned(true);
    f.sc.onStepKey(3, true, 300);
    f.sc.onStepKey(3, false, 350);
    CHECK(f.cell(34, 2).empty());
}

TEST_CASE("seq.steps: hold + turn writes a lock on the held step, the release does not toggle", "[seq][steps]")
{
    Fixture f;
    f.sc.onStepKey(3, true, 0);
    CHECK(f.sc.heldStep() == 3);
    CHECK(f.sc.interceptEncoder(spec("playback.pitch"), 4, false));
    CHECK(f.sc.interceptEncoder(spec("playback.pitch"), 1, false));
    CHECK(f.sc.interceptEncoder(spec("track.chokeGroup"), 1, false)); // not lockable: consumed, greyed (UI-204)
    f.sc.onStepKey(3, false, 400);
    float v = 0;
    REQUIRE(f.sc.lockOf(spec("playback.pitch"), 3, v));
    CHECK(v == 5.0f); // 5 coarse detents from the track's 0
    CHECK(f.cell(2, 2).triggerType == TriggerType::LockOnly); // our choice: the empty step became a PARAM step
    CHECK(core::commands::history(f.bus).undoDepth() == 1u); // the turn coalesced
    CHECK_FALSE(f.sc.interceptEncoder(spec("playback.pitch"), 1, false)); // no step held: the normal path

    // The decorator gives the shell's hold + turn the same model.
    ui::LocalParamValues inner(core::builtinParams());
    steps::StepLockAccess access(inner, f.sc);
    const std::uint16_t scope = 0x02;
    REQUIRE(access.lockOf(spec("playback.pitch"), scope, 3, v));
    access.setLock(spec("env.attack"), scope, 3, 40.0f);
    REQUIRE(f.sc.lockOf(spec("env.attack"), 3, v));
    CHECK(v == 40.0f);
}

TEST_CASE("seq.steps: recorded hits and erased steps of a pass are one undo step; REC and ERASE reach the engine",
          "[seq][steps][record]")
{
    Fixture f;
    f.bus.execute(core::commands::sequencer::toggleStep(0, 0, 2, 8, TriggerType::Full, 127));
    const auto before = std::make_unique<core::model::Pattern>(f.pat());
    const auto depth = core::commands::history(f.bus).undoDepth();
    f.sc.setTransport(false, true);
    CHECK(f.posted("seq.record") == 1.0f);
    f.sc.setTransport(true, true);
    using K = engine::EngineEvent::Kind;
    f.sc.onEngineEvent({K::RecordedHit, 2, 4, 100, 0});
    f.sc.onEngineEvent({K::RecordedHit, 2, 6, 90, 16});
    f.sc.onEngineEvent({K::RecordedHit, 9, 0, 70, -3});
    f.sc.setErase(true);
    CHECK(f.posted("seq.erase", 2) == 1.0f);
    f.sc.onEngineEvent({K::ErasedStep, 2, 8, 0, 0});
    f.sc.setErase(false);
    CHECK(f.sc.groupOpen()); // still recording
    // SEQ-023: an encoder turn while recording locks the playing step, in the same group — with MOTION REC ON only.
    CHECK_FALSE(f.sc.recordEncoder(spec("track.volume"), 77.0f, 6));
    f.sc.setMotionRec(true);
    CHECK(f.sc.recordEncoder(spec("track.volume"), 77.0f, 6));
    f.sc.setTransport(false, true); // STOP: the pass ends
    CHECK_FALSE(f.sc.groupOpen());
    CHECK(core::commands::history(f.bus).undoDepth() == depth + 1);
    CHECK(f.cell(4, 2).value2 == 100);
    CHECK(f.cell(0, 9).trigger == 15);
    CHECK(f.cell(8, 2).empty());
    float v = 0;
    CHECK(core::commands::sequencer::lockOf(f.pat(), 6, 2, 0, v));
    CHECK(v == 77.0f);
    REQUIRE(f.bus.undo());
    CHECK(f.pat() == *before);
    CHECK_FALSE(f.sc.recordEncoder(spec("track.volume"), 70.0f, 6)); // not recording now
}

TEST_CASE("seq.steps: MOTION REC overwrites the steps the playhead passes while the knob turns; the motion view",
          "[seq][steps][record]")
{
    Fixture f;
    f.sc.setMotionRec(true);
    f.sc.setTransport(true, true);
    const auto depth = core::commands::history(f.bus).undoDepth();
    f.sc.tick(1000);
    REQUIRE(f.sc.recordEncoder(spec("track.volume"), 50.0f, 2));
    f.sc.tick(1100); // still turning: steps 3–4 were passed, they get the new value too
    REQUIRE(f.sc.recordEncoder(spec("track.volume"), 60.0f, 5));
    f.sc.tick(2000); // a pause: only the playing step
    REQUIRE(f.sc.recordEncoder(spec("track.volume"), 70.0f, 9));
    float v = 0;
    const auto lockAt = [&](int step) { return core::commands::sequencer::lockOf(f.pat(), step, 2, 0, v) ? v : -1.0f; };
    CHECK(lockAt(2) == 50.0f);
    CHECK(lockAt(3) == 60.0f);
    CHECK(lockAt(4) == 60.0f);
    CHECK(lockAt(5) == 60.0f);
    CHECK(lockAt(6) == -1.0f);
    CHECK(lockAt(9) == 70.0f);
    CHECK(f.cell(3, 2).triggerType == TriggerType::LockOnly);
    f.sc.setTransport(false, true);
    CHECK(core::commands::history(f.bus).undoDepth() == depth + 1); // one undo step, one pattern copy

    // The motion view: locked steps lit in the locked tint, a plain trigger dark; the default view is unchanged.
    f.bus.execute(core::commands::sequencer::toggleStep(0, 0, 2, 12, TriggerType::Full, 127));
    engine::PlayheadFeed ph;
    f.sc.tick(10000);
    Lamps normal;
    f.sc.paintStepLamps(normal, ph);
    CHECK(normal.lit[io::step(13)].second == 255);
    f.sc.setMotionView(true);
    Lamps motion;
    f.sc.paintStepLamps(motion, ph);
    CHECK(motion.lit[io::step(13)].second == 0);
    CHECK(motion.lit[io::step(4)].second == 255);
    CHECK(motion.lit[io::step(4)].first == normal.lit[io::step(4)].first);
    CHECK(motion.lit[io::step(7)].second == 0);
}

TEST_CASE("seq.steps: step lamps show triggers in the track colour, HALF dim, the playing and held steps white",
          "[seq][steps]")
{
    Fixture f;
    f.bus.execute(core::commands::sequencer::toggleStep(0, 0, 2, 0, TriggerType::Full, 127));
    f.bus.execute(core::commands::sequencer::toggleStep(0, 0, 2, 1, TriggerType::Half, 127));
    f.bus.execute(core::commands::sequencer::toggleStep(0, 0, 2, 2, TriggerType::LockOnly, 127));
    engine::PlayheadFeed ph;
    ph.playing = true;
    ph.step = 5;
    Lamps lamps;
    f.sc.paintStepLamps(lamps, ph);
    REQUIRE(lamps.lit.size() == 16u);
    CHECK(lamps.lit[io::step(1)].second == 255);
    CHECK(lamps.lit[io::step(2)].second == io::kDimLevel);
    CHECK(lamps.lit[io::step(3)].second > 0);
    CHECK(lamps.lit[io::step(3)].second < io::kDimLevel);
    CHECK(lamps.lit[io::step(4)].second == 0);
    CHECK(lamps.lit[io::step(6)].first == io::lampcolour::white);
    CHECK(lamps.lit[io::step(1)].first != io::lampcolour::white);
    Lamps none;
    f.sc.setStepKeysOwned(true);
    f.sc.paintStepLamps(none, ph);
    CHECK(none.lit.empty());
}

TEST_CASE("seq.steps: the pattern sync posts the edited pattern as an ObjectSwap; an edit is heard on the next pass",
          "[seq][steps]")
{
    Fixture f;
    f.bus.execute(core::commands::sequencer::toggleStep(1, 3, 0, 0, TriggerType::Full, 127));
    core::ChangeSet changes;
    changes.add(core::commands::sequencer::patternPath(1, 3));
    changes.add("scene[1].track[0].volume"); // not a pattern
    steps::forwardPatternChanges(changes, core::commands::project(f.bus), f.bridge);
    REQUIRE(f.bridge.posted.size() == 1u);
    const auto* swap = std::get_if<engine::ObjectSwap>(&f.bridge.posted[0]);
    REQUIRE(swap != nullptr);
    CHECK(swap->slot.kind == engine::SwapSlot::Pattern);
    CHECK((swap->slot.scene == 1 && swap->slot.index == 3));
    const auto* d = static_cast<const engine::sequencer::PatternData*>(swap->object);
    CHECK(d->triggerMask[0] == 1u);
    core::ChangeSet scene;
    scene.add("scene[2]");
    steps::forwardPatternChanges(scene, core::commands::project(f.bus), f.bridge);
    CHECK(f.bridge.posted.size() == 17u);
    // A refused post destroys the object (no leak, no crash).
    f.bridge.accept = false;
    CHECK_FALSE(steps::postPattern(f.bridge, core::commands::project(f.bus), 0, 0));
    f.bridge.accept = true;

    // forwardAllSequencer: 256 patterns, swing (stored 50–75 → shown 0–50) and the tracks' OFF SET.
    fakes::FakeEngineBridge all;
    auto& proj = core::commands::project(f.bus);
    proj.settings.swing = 75;
    proj.scenes[0].tracks[5].seq.offset = -6;
    steps::forwardAllSequencer(proj, all);
    int swaps = 0;
    float swing = -1, offset = 99;
    for (const auto& m : all.posted)
    {
        if (const auto* s = std::get_if<engine::ObjectSwap>(&m))
        {
            ++swaps;
            engine::sequencer::destroyPattern(s->object);
        }
        else if (const auto* c = std::get_if<engine::ParamChange>(&m))
        {
            if (c->param == seqtest::id("seq.swing"))
                swing = c->value;
            if (c->param == seqtest::id("seq.offset") && c->scope == 0x05)
                offset = c->value;
        }
    }
    CHECK(swaps == 256);
    CHECK(swing == 50.0f);
    CHECK(offset == -6.0f);
    CHECK(steps::swingStoredFromShown(25.0f) == 63);
}

TEST_CASE("seq.steps: step entry through the bus and the bridge hook is heard by the engine on the next pass",
          "[seq][steps][engine]")
{
    seqtest::Rig rig;
    core::CommandBus bus;
    core::commands::replaceProject(bus, core::model::newDefaultProject());
    core::commands::setBridge(bus, [&](const core::ChangeSet& c, const core::model::Project& p) {
        steps::forwardPatternChanges(c, p, rig.e->bridge());
    });
    steps::StepController sc(bus, &rig.e->bridge());
    steps::setShownBar(0);
    sc.setContext(0, 0, 4);
    REQUIRE(rig.play());
    const double st = seqtest::stepFrames();
    rig.until(static_cast<long>(2.5 * st));
    sc.onStepKey(2, true, 0); // step 2 already passed in this pass
    sc.onStepKey(2, false, 50);
    sc.onStepKey(4, true, 60); // step 4 still ahead
    sc.onStepKey(4, false, 90);
    rig.until(static_cast<long>(18 * st));
    const auto hits = rig.sequenced(4);
    REQUIRE(hits.size() == 2u);
    CHECK(hits[0].frame == static_cast<long>(std::floor(3 * st)));  // step 4, this pass
    CHECK(hits[1].frame == static_cast<long>(std::floor(17 * st))); // step 2, next pass (SEQ-061)
}
