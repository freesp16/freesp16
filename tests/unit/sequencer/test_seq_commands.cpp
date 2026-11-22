// The sequencer's message-thread half (core/commands/sequencer, core/util/sequencer): step entry, locks, recorded hits,
// clear and post-quantize as undoable commands (EDT-U1/U2/U3), the lock slot table (OQ-01/07), the pattern image the
// engine builds from, record quantise arithmetic (SEQ-042), and the sequencer.cpp ParamSpecs.
#include "core/commands/CommandBusAccess.h"
#include "core/commands/UndoStack.h"
#include "core/commands/sequencer/SequencerCommands.h"
#include "core/model/ModelOps.h"
#include "core/model/Project.h"
#include "core/params/ParamRegistry.h"
#include "core/util/sequencer/RecordQuantize.h"
#include "engine/sequencer/PatternBuilder.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <memory>

namespace cmd = core::commands::sequencer;
using core::model::TriggerType;

namespace
{
struct Bus
{
    core::CommandBus bus;
    Bus() { core::commands::replaceProject(bus, core::model::newDefaultProject()); }
    core::model::Project& p() { return core::commands::project(bus); }
    core::model::Pattern& pat(int s = 0, int q = 0) { return p().scenes[static_cast<std::size_t>(s)].patterns[static_cast<std::size_t>(q)]; }
    const core::model::Step& cell(int step, int track, int s = 0, int q = 0)
    {
        return pat(s, q).steps[static_cast<std::size_t>(step)][static_cast<std::size_t>(track)];
    }
};
const core::ParamSpec& spec(const char* id)
{
    const auto* s = core::builtinParams().find(id);
    REQUIRE(s != nullptr);
    return *s;
}
} // namespace

TEST_CASE("seq.commands: toggleStep writes FULL / HALF / PARAM with velocity and toggles back; undo is exact",
          "[seq][commands]")
{
    Bus b;
    const auto before = std::make_unique<core::model::Pattern>(b.pat(2, 5));
    b.bus.execute(cmd::toggleStep(2, 5, 3, 10, TriggerType::Half, 99));
    CHECK(b.cell(10, 3, 2, 5).trigger == 11);
    CHECK(b.cell(10, 3, 2, 5).triggerType == TriggerType::Half);
    CHECK(b.cell(10, 3, 2, 5).value2 == 99);
    b.bus.execute(cmd::toggleStep(2, 5, 3, 11, TriggerType::LockOnly, 127));
    CHECK(b.cell(11, 3, 2, 5).trigger == 8);
    b.bus.execute(cmd::toggleStep(2, 5, 3, 12, TriggerType::Full, 300));
    CHECK(b.cell(12, 3, 2, 5).trigger == 15);
    CHECK(b.cell(12, 3, 2, 5).value2 == 127);
    b.bus.execute(cmd::toggleStep(2, 5, 3, 10, TriggerType::Full, 127)); // toggles off
    CHECK(b.cell(10, 3, 2, 5).trigger == core::model::Step::kEmpty);
    for (int i = 0; i < 4; ++i)
        REQUIRE(b.bus.undo()); // step toggles never coalesce
    CHECK(b.pat(2, 5) == *before);
    REQUIRE(b.bus.redo());
    CHECK(b.cell(10, 3, 2, 5).trigger == 11);
}

TEST_CASE("seq.commands: setLock / removeLock / clearStep; hold + turn coalesces; locks travel with clears",
          "[seq][commands]")
{
    Bus b;
    b.bus.execute(cmd::toggleStep(0, 0, 0, 2, TriggerType::Full, 127));
    const auto base = std::make_unique<core::model::Pattern>(b.pat());
    b.bus.execute(cmd::setLock(0, 0, 0, 2, 4, 3.0f)); // PITCH
    b.bus.execute(cmd::setLock(0, 0, 0, 2, 4, 5.0f)); // the same knob, turned again (coalesces, EDT-U2)
    float v = 0;
    REQUIRE(cmd::lockOf(b.pat(), 2, 0, 4, v));
    CHECK(v == 5.0f);
    REQUIRE(b.bus.undo());
    CHECK(b.pat() == *base); // one undo step for the whole turn
    REQUIRE(b.bus.redo());
    b.bus.execute(cmd::setLock(0, 0, 0, 2, 0, 90.0f));
    b.bus.execute(cmd::removeLock(0, 0, 0, 2, 4));
    CHECK_FALSE(cmd::lockOf(b.pat(), 2, 0, 4, v));
    REQUIRE(cmd::lockOf(b.pat(), 2, 0, 0, v));
    CHECK(b.pat().locks.size() == 1u);
    b.bus.execute(cmd::clearStep(0, 0, 0, 2));
    CHECK(b.cell(2, 0).empty());
    CHECK(b.pat().locks.empty()); // compacted
    // Our choice: a lock on an empty step makes it a PARAM step.
    b.bus.execute(cmd::setLock(0, 0, 1, 7, 5, 64.0f));
    CHECK(b.cell(7, 1).triggerType == TriggerType::LockOnly);
    CHECK(b.cell(7, 1).trigger == 8);
}

TEST_CASE("seq.commands: one recording pass is one undo step (EDT-U3)", "[seq][commands][record]")
{
    Bus b;
    b.bus.execute(cmd::toggleStep(0, 0, 2, 0, TriggerType::Full, 127));
    const auto before = std::make_unique<core::model::Pattern>(b.pat());
    const auto depth = core::commands::history(b.bus).undoDepth();
    core::commands::beginGroup(b.bus);
    b.bus.execute(cmd::recordHit(0, 0, 1, 4, 100, 0));
    b.bus.execute(cmd::recordHit(0, 0, 1, 5, 80, 16));
    b.bus.execute(cmd::recordHit(0, 0, 3, 9, 70, -6));
    b.bus.execute(cmd::clearStep(0, 0, 2, 0)); // ERASE in the same pass
    core::commands::endGroup(b.bus);
    CHECK(core::commands::history(b.bus).undoDepth() == depth + 1);
    CHECK(b.cell(4, 1).value2 == 100);
    float v = 0;
    CHECK_FALSE(cmd::lockOf(b.pat(), 4, 1, 15, v));
    REQUIRE(cmd::lockOf(b.pat(), 5, 1, 15, v));
    CHECK(v == 16.0f);
    REQUIRE(cmd::lockOf(b.pat(), 9, 3, 15, v));
    CHECK(v == -6.0f);
    CHECK(b.cell(0, 2).empty());
    REQUIRE(b.bus.undo());
    CHECK(b.pat() == *before);
    // Overdub: a second hit on a recorded step replaces velocity and timing.
    b.bus.execute(cmd::recordHit(0, 0, 1, 5, 80, 16));
    b.bus.execute(cmd::recordHit(0, 0, 1, 5, 60, 0));
    CHECK(b.cell(5, 1).value2 == 60);
    CHECK_FALSE(cmd::lockOf(b.pat(), 5, 1, 15, v));
}

TEST_CASE("seq.commands: clearTriggers and quantizeOffsets (post-quantize strength)", "[seq][commands]")
{
    Bus b;
    for (int s : {1, 2, 3})
        b.bus.execute(cmd::recordHit(0, 1, 0, s, 100, 12));
    const int steps[] = {1, 2};
    b.bus.execute(cmd::quantizeOffsets(0, 1, 0, steps, 0.5f));
    float v = 0;
    REQUIRE(cmd::lockOf(b.pat(0, 1), 1, 0, 15, v));
    CHECK(v == 6.0f);
    b.bus.execute(cmd::quantizeOffsets(0, 1, 0, steps, 1.0f));
    CHECK_FALSE(cmd::lockOf(b.pat(0, 1), 2, 0, 15, v));
    REQUIRE(cmd::lockOf(b.pat(0, 1), 3, 0, 15, v)); // not in the list
    const int all[] = {1, 2, 3, 99};
    b.bus.execute(cmd::clearTriggers(0, 1, 0, all));
    for (int s : {1, 2, 3})
        CHECK(b.cell(s, 0, 0, 1).empty());
    CHECK(b.pat(0, 1).locks.empty());
}

TEST_CASE("seq.commands: setLocks on target steps and shiftSteps rotate with their locks", "[seq][commands]")
{
    Bus b;
    b.bus.execute(cmd::toggleStep(0, 0, 4, 0, TriggerType::Full, 127));
    b.bus.execute(cmd::setLock(0, 0, 4, 0, 16, 3.0f));
    const int steps[] = {0, 1};
    const float values[] = {1.0f, 4.0f};
    b.bus.execute(cmd::setLocks(0, 0, 4, steps, 16, values, 1.0f, "RETRIGGER CNT")); // 1 removes
    float v = 0;
    CHECK_FALSE(cmd::lockOf(b.pat(), 0, 4, 16, v));
    REQUIRE(cmd::lockOf(b.pat(), 1, 4, 16, v));
    CHECK(v == 4.0f);
    b.bus.execute(cmd::shiftSteps(0, 0, 4, 0, 16, 2));
    CHECK(b.cell(2, 4).trigger == 15);
    REQUIRE(cmd::lockOf(b.pat(), 3, 4, 16, v));
    b.bus.execute(cmd::shiftSteps(0, 0, 4, 0, 16, -3));
    CHECK(b.cell(15, 4).trigger == 15);
}

TEST_CASE("seq.commands: paths, the lock slot table and the pattern image", "[seq][commands]")
{
    int s = -1, q = -1;
    CHECK(cmd::patternPath(3, 12) == "scene[3].pattern[12]");
    CHECK(cmd::parsePatternPath("scene[3].pattern[12]", s, q));
    CHECK((s == 3 && q == 12));
    CHECK(cmd::parsePatternPath("scene[15].pattern[0].step[3].track[2].pitch", s, q));
    CHECK_FALSE(cmd::parsePatternPath("scene[3].track[2].volume", s, q));
    CHECK_FALSE(cmd::parsePatternPath("scene[16].pattern[0]", s, q));

    CHECK(cmd::lockSlotOf(spec("track.volume")) == 0);
    CHECK(cmd::lockSlotOf(spec("playback.pitch")) == 4);
    CHECK(cmd::lockSlotOf(spec("env.release")) == 14);
    CHECK(cmd::lockSlotOf(spec("seq.offset")) == 15);
    CHECK(cmd::lockSlotOf(spec("seq.retrigSpeed")) == 17);
    CHECK(cmd::lockSlotOf(spec("seq.swing")) == -1); // not lockable
    // Insert FX knobs (OQ-07: both slots): slot 23 + k / 29 + k by internal index.
    int fx1 = 0, fx2 = 0;
    for (const auto& p : core::builtinParams().all())
    {
        const int slot = cmd::lockSlotOf(p);
        if (p.id.substr(0, 4) == "fx1." && p.lockable)
        {
            CHECK((slot >= 23 && slot <= 28));
            ++fx1;
        }
        if (p.id.substr(0, 4) == "fx2." && p.lockable)
        {
            CHECK((slot >= 29 && slot <= 34));
            ++fx2;
        }
        if (p.id.substr(0, 7) == "master." || p.id.substr(0, 5) == "send.")
            CHECK(slot == -1); // SEQ-028
    }
    CHECK(fx1 == fx2);

    auto p = std::make_unique<core::model::Pattern>();
    p->length = 24;
    p->trackMutes[3] = true;
    auto& c = p->steps[5][2];
    c.trigger = 11;
    c.triggerType = TriggerType::Half;
    c.value2 = 64;
    p->locks.emplace_back();
    p->locks[0].mask = 1ull << 15 | 1ull << 16 | 1ull << 4;
    p->locks[0].values[15] = -7;
    p->locks[0].values[16] = 3;
    p->locks[0].values[4] = 12;
    p->locks[0].change[4] = core::model::LockChange::Hold;
    c.lockIndex = 0;
    auto img = std::make_unique<core::sequencer::PatternImage>();
    cmd::makePatternImage(*p, *img);
    CHECK(img->length == 24);
    CHECK(img->trackMutes == 1u << 3);
    const auto& ic = img->cells[5][2];
    CHECK(ic.velocity == 64);
    CHECK(ic.kind == 1);
    CHECK(ic.offset24 == -7);
    CHECK(ic.retrigCount == 3);
    CHECK(ic.retrigSpeed == 0);
    CHECK(img->cells[6][2].velocity == 0);

    // The engine's builder (registered at start-up) makes the 8-byte cells, the trigger masks and the LockSets.
    const auto* d = engine::sequencer::buildPattern(*img);
    REQUIRE(d != nullptr);
    CHECK(d->length == 24);
    CHECK(d->triggerMask[2] == 1ull << 5);
    CHECK(d->cells[5][2].kind == engine::trigger::TriggerKind::Half);
    REQUIRE(d->locks.size() == 1u);
    float v = 0;
    CHECK(d->locks[0].get(4, v));
    CHECK(v == 12.0f);
    CHECK(d->locks[0].change[4] == 1);
    CHECK(engine::sequencer::hasOffsetLock(*d, d->cells[5][2]));
    CHECK(core::sequencer::patternCompiler().build != nullptr);
    engine::sequencer::destroyPattern(d);
}

TEST_CASE("seq.quantize: record grids in 1/24 steps (SEQ-042)", "[seq][record]")
{
    using core::sequencer::RecordQuantize;
    using core::sequencer::quantizeHit;
    auto q = quantizeHit(0.66, 16, RecordQuantize::SixteenthTriplet, 0.0);
    CHECK((q.step == 0 && q.offset24 == 16));
    q = quantizeHit(1.40, 16, RecordQuantize::SixteenthTriplet, 0.0);
    CHECK((q.step == 1 && q.offset24 == 8));
    q = quantizeHit(3.9, 16, RecordQuantize::Sixteenth, 0.0);
    CHECK((q.step == 4 && q.offset24 == 0));
    q = quantizeHit(3.3, 16, RecordQuantize::ThirtySecond, 0.0);
    CHECK((q.step == 3 && q.offset24 == 12));
    q = quantizeHit(2.9, 16, RecordQuantize::Eighth, 0.0);
    CHECK((q.step == 2 && q.offset24 == 0));
    q = quantizeHit(15.8, 16, RecordQuantize::Sixteenth, 0.0);
    CHECK((q.step == 0 && q.wrapped));
    q = quantizeHit(6.25, 16, RecordQuantize::Off, 0.0);
    CHECK((q.step == 6 && q.offset24 == 6));
    q = quantizeHit(1.3, 16, RecordQuantize::Off, 0.25); // swing compensated: step 2 at 1.25 + 1/24 × 1.2
    CHECK((q.step == 1 && q.offset24 == 1));
    CHECK(core::sequencer::gridIn24ths(RecordQuantize::EighthTriplet) == 32);
}

TEST_CASE("seq.params: sequencer.cpp registers the sequencer ids with their CCs and lock flags", "[seq][params]")
{
    const auto& r = core::builtinParams();
    for (const char* id : {"seq.swing", "seq.recordQuantize", "seq.triggerType", "seq.offset", "seq.retrigCount",
                           "seq.retrigSpeed", "seq.shift", "seq.shiftArea", "seq.transpose", "seq.padStartsSequence",
                           "seq.postQuantize", "seq.erase", "seq.record"})
    {
        INFO(id);
        const auto* s = r.find(id);
        REQUIRE(s != nullptr);
        CHECK(s->featureFlag.empty()); // the real ones, not the screens' interim stubs
    }
    CHECK(spec("seq.offset").lockable);
    CHECK(spec("seq.offset").midiCc == 54);
    CHECK(spec("seq.retrigCount").midiCc == 55);
    CHECK(spec("seq.retrigSpeed").midiCc == 56);
    CHECK(spec("seq.recordQuantize").enumLabels.size() == 6u);
    CHECK(spec("seq.recordQuantize").defaultValue == 3.0f); // 1/16
    CHECK(spec("seq.swing").maxValue == 50.0f);
    CHECK(spec("seq.erase").isPreference()); // internal, never saved in a project
}

TEST_CASE("seq.commands: editMotion writes and removes motion, clearMotion clears lanes; PARAM steps left empty go",
          "[seq][commands][motion]")
{
    Bus b;
    const auto before = std::make_unique<core::model::Pattern>(b.pat());
    b.bus.execute(cmd::toggleStep(0, 0, 3, 4, TriggerType::Full, 100));
    const int steps[] = {2, 4, 6};
    const float values[] = {10.0f, 20.0f, 30.0f};
    b.bus.execute(cmd::editMotion(0, 0, 3, steps, 0, values));
    float v = 0;
    REQUIRE(cmd::lockOf(b.pat(), 2, 3, 0, v));
    CHECK(v == 10.0f);
    CHECK(b.cell(2, 3).triggerType == TriggerType::LockOnly); // an empty step became a PARAM step
    CHECK(b.cell(4, 3).triggerType == TriggerType::Full);     // a trigger keeps its type
    b.bus.execute(cmd::editMotion(0, 0, 3, steps, 5, values));

    // NaN removes; a PARAM step left without locks is empty again, a trigger stays.
    const int one[] = {6};
    const float gone[] = {std::numeric_limits<float>::quiet_NaN()};
    b.bus.execute(cmd::editMotion(0, 0, 3, one, 0, gone));
    CHECK_FALSE(cmd::lockOf(b.pat(), 6, 3, 0, v));
    CHECK(cmd::lockOf(b.pat(), 6, 3, 5, v)); // the other slot's lock stays
    CHECK_FALSE(b.cell(6, 3).empty());

    b.bus.execute(cmd::clearMotion(0, 0, 3, std::uint64_t {1} << 5));
    CHECK(b.cell(6, 3).empty());
    CHECK(cmd::lockOf(b.pat(), 2, 3, 0, v));
    b.bus.execute(cmd::clearMotion(0, 0, 3, ~std::uint64_t {0}));
    CHECK(b.cell(2, 3).empty());
    CHECK(b.cell(4, 3).trigger != core::model::Step::kEmpty); // the hit stays, without locks
    CHECK(b.cell(4, 3).lockIndex == core::model::Step::kNoLocks);

    while (b.bus.undo())
    {
    }
    CHECK(b.pat() == *before);
}
