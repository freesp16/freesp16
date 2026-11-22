// Step shift and SEQUENCE targeting as undoable commands (spec 02-features/sequencer.md SEQ-036 / SEQ-038,
// state-and-undo.md EDT-U1/U2): BAR / ALL rotation with the locks travelling, +n then −n = identity, a turn of
// several detents undoes in one step, the track value with no step selected, locks on exactly the selected steps.
#include "core/commands/CommandBusAccess.h"
#include "core/commands/UndoStack.h"
#include "core/commands/perf/ShiftSteps.h"
#include "core/commands/sequencer/SequencerCommands.h"
#include "core/model/ModelOps.h"
#include "core/model/Project.h"
#include "core/params/ParamRegistry.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <random>

namespace seqcmd = core::commands::sequencer;
using core::model::Pattern;
using core::perf::ShiftArea;

namespace
{
struct Bus
{
    core::CommandBus bus;
    Bus() { core::commands::replaceProject(bus, core::model::newDefaultProject()); }
    core::model::Project& p() { return core::commands::project(bus); }
    Pattern& pat(int s = 0, int q = 0) { return p().scenes[static_cast<std::size_t>(s)].patterns[static_cast<std::size_t>(q)]; }
    const core::model::Step& cell(int step, int track, int s = 0, int q = 0)
    {
        return pat(s, q).steps[static_cast<std::size_t>(step)][static_cast<std::size_t>(track)];
    }
    std::size_t depth() const { return core::commands::history(bus).undoDepth(); }
};
core::ParamId id(const char* name) { return core::builtinParams().idOf(name).value_or(core::ParamId {}); }

// A dense random pattern on track 6 (and a neighbour track that must not move): triggers, kinds, velocities and up to
// 35 locks per cell.
void fill(Pattern& p, int length, unsigned seed)
{
    std::mt19937 rng(seed);
    p.length = length;
    for (int s = 0; s < 64; ++s)
        for (const int t : {5, 6})
        {
            auto& c = p.steps[static_cast<std::size_t>(s)][static_cast<std::size_t>(t)];
            if (rng() % 3 == 0)
                continue;
            const int kind = static_cast<int>(rng() % 3);
            c.triggerType = kind == 0 ? core::model::TriggerType::Full
                                      : (kind == 1 ? core::model::TriggerType::Half : core::model::TriggerType::LockOnly);
            c.trigger = static_cast<std::int16_t>(kind == 0 ? 15 : (kind == 1 ? 11 : 8));
            c.value2 = static_cast<std::int16_t>(1 + rng() % 127);
            if (rng() % 2 == 0)
            {
                core::model::StepLocks l;
                for (int k = 0; k < 35; ++k)
                    if (rng() % 2 == 0)
                    {
                        l.mask |= std::uint64_t {1} << k;
                        l.values[static_cast<std::size_t>(k)] = static_cast<float>(rng() % 100);
                    }
                p.locks.push_back(l);
                c.lockIndex = static_cast<std::uint16_t>(p.locks.size() - 1);
            }
        }
}
} // namespace

TEST_CASE("perf.shift: the shift range is the bar shown (clipped to the length) or the whole pattern", "[perf][shift]")
{
    using core::perf::shiftRange;
    CHECK(shiftRange(64, ShiftArea::All, 2).first == 0);
    CHECK(shiftRange(64, ShiftArea::All, 2).count == 64);
    CHECK(shiftRange(24, ShiftArea::All, 0).count == 24);
    CHECK(shiftRange(64, ShiftArea::Bar, 2).first == 32);
    CHECK(shiftRange(64, ShiftArea::Bar, 2).count == 16);
    CHECK(shiftRange(24, ShiftArea::Bar, 1).count == 8); // steps 17-24
    CHECK(shiftRange(24, ShiftArea::Bar, 2).count == 0); // a bar past the end: nothing
}

TEST_CASE("perf.shift: ALL rotates the track's triggers and locks circularly; other tracks stay", "[perf][shift]")
{
    Bus b;
    fill(b.pat(1, 2), 32, 7);
    const Pattern before = b.pat(1, 2);
    b.bus.execute(core::perf::shiftSteps(1, 2, 6, ShiftArea::All, 0, 3));
    const Pattern& after = b.pat(1, 2);
    for (int s = 0; s < 32; ++s)
    {
        INFO("step " << s);
        CHECK(after.steps[static_cast<std::size_t>((s + 3) % 32)][6] == before.steps[static_cast<std::size_t>(s)][6]);
        CHECK(after.steps[static_cast<std::size_t>(s)][5] == before.steps[static_cast<std::size_t>(s)][5]);
    }
    for (int s = 32; s < 64; ++s) // beyond the length: untouched
        CHECK(after.steps[static_cast<std::size_t>(s)][6] == before.steps[static_cast<std::size_t>(s)][6]);
    CHECK(after.locks == before.locks); // the lock entries are unchanged: their cells moved
    float v0 = 0, v1 = 0;
    for (int s = 0; s < 32; ++s)
        CHECK(seqcmd::lockOf(before, s, 6, 4, v0) == seqcmd::lockOf(after, (s + 3) % 32, 6, 4, v1));
}

TEST_CASE("perf.shift: BAR / ALL round-trip, +n then -n is the identity (locks included) for every n",
          "[perf][shift]")
{
    for (const auto area : {ShiftArea::Bar, ShiftArea::All})
        for (const int length : {16, 24, 64})
            for (const int bar : {0, 1, 3})
                for (int n = -17; n <= 17; n += 3)
                {
                    INFO("area " << static_cast<int>(area) << " length " << length << " bar " << bar << " n " << n);
                    Bus b;
                    fill(b.pat(), length, static_cast<unsigned>(length * 131 + bar * 7 + n + 40));
                    const Pattern before = b.pat();
                    b.bus.execute(core::perf::shiftSteps(0, 0, 6, area, bar, n));
                    const auto r = core::perf::shiftRange(length, area, bar);
                    if (r.count == 0 || n % r.count == 0)
                        CHECK(b.pat() == before); // nothing to rotate
                    b.bus.execute(core::perf::shiftSteps(0, 0, 6, area, bar, -n));
                    CHECK(b.pat() == before);
                }
}

TEST_CASE("perf.shift: a turn of several detents is one undo step, bit-identical; redo replays it", "[perf][shift]")
{
    Bus b;
    fill(b.pat(3, 4), 64, 99);
    const Pattern before = b.pat(3, 4);
    const auto depth0 = b.depth();
    for (int i = 0; i < 5; ++i)
        b.bus.execute(core::perf::shiftSteps(3, 4, 6, ShiftArea::Bar, 1, i == 3 ? -1 : 1)); // +1 +1 +1 −1 +1 = +3
    const Pattern shifted = b.pat(3, 4);
    Bus ref;
    fill(ref.pat(3, 4), 64, 99);
    ref.bus.execute(core::perf::shiftSteps(3, 4, 6, ShiftArea::Bar, 1, 3));
    CHECK(shifted == ref.pat(3, 4));
    CHECK(b.depth() == depth0 + 1); // EDT-U2: coalesced
    REQUIRE(b.bus.undo());
    CHECK(b.pat(3, 4) == before);
    REQUIRE(b.bus.redo());
    CHECK(b.pat(3, 4) == shifted);
    // A shift of another track is a separate step.
    b.bus.execute(core::perf::shiftSteps(3, 4, 5, ShiftArea::Bar, 1, 1));
    CHECK(b.depth() == depth0 + 2);
}

TEST_CASE("perf.shift: the change set names the pattern (the bridge rebuilds its object)", "[perf][shift]")
{
    const auto c = core::perf::shiftSteps(2, 9, 0, ShiftArea::All, 0, 1);
    core::ChangeSet cs;
    c->collectChanges(cs);
    REQUIRE(cs.size() == 1u);
    CHECK(cs.paths()[0] == "scene[2].pattern[9]");
    CHECK(c->label() == "SHIFT");
}

TEST_CASE("perf.target: no step selected sets the track value; selected steps get locks on exactly those steps",
          "[perf][target]")
{
    Bus b;
    // SEQ-038, no selection: the track's RETRIGGER CNT (model TrackSequencerSettings), undoable, coalescing.
    auto& seq = b.p().scenes[2].tracks[7].seq;
    const int before = seq.retriggerCount;
    auto c = core::perf::setStepsParam(2, 0, 7, 0, id("seq.retrigCount"), 5.0f);
    REQUIRE(c != nullptr);
    core::ChangeSet cs;
    c->collectChanges(cs);
    CHECK(cs.paths()[0] == "scene[2].track[7].seq.retrigCount");
    b.bus.execute(std::move(c));
    CHECK(seq.retriggerCount == 5);
    b.bus.execute(core::perf::setStepsParam(2, 0, 7, 0, id("seq.retrigCount"), 40.0f)); // clamped to 16
    CHECK(seq.retriggerCount == 16);
    b.bus.execute(core::perf::setStepsParam(2, 0, 7, 0, id("seq.retrigSpeed"), 5.0f));
    b.bus.execute(core::perf::setStepsParam(2, 0, 7, 0, id("seq.offset"), -7.4f));
    b.bus.execute(core::perf::setStepsParam(2, 0, 7, 0, id("seq.transpose"), 12.0f));
    CHECK(seq.retriggerSpeed == 5);
    CHECK(seq.offset == -7);
    CHECK(seq.transpose == 12);
    for (int i = 0; i < 4; ++i)
        REQUIRE(b.bus.undo());
    CHECK(seq.retriggerCount == before); // the two CNT turns coalesced

    // Steps selected: locks on steps 1, 5 and 64 only, including a value equal to the default.
    const Pattern empty = b.pat(0, 3);
    const std::uint64_t mask = (1ull << 0) | (1ull << 4) | (1ull << 63);
    b.bus.execute(core::perf::setStepsParam(0, 3, 2, mask, id("seq.retrigCount"), 1.0f));
    b.bus.execute(core::perf::setStepsParam(0, 3, 2, mask, id("seq.offset"), 12.0f));
    for (int s = 0; s < 64; ++s)
    {
        INFO("step " << s);
        float v = 0;
        const bool selected = (mask >> s & 1u) != 0;
        CHECK(seqcmd::lockOf(b.pat(0, 3), s, 2, 16, v) == selected);
        if (selected)
            CHECK(v == 1.0f);
        CHECK(seqcmd::lockOf(b.pat(0, 3), s, 2, 15, v) == selected);
        if (selected)
            CHECK(v == 12.0f);
    }
    REQUIRE(b.bus.undo());
    REQUIRE(b.bus.undo());
    CHECK(b.pat(0, 3) == empty);

    // Nothing to target: transpose has no lock slot, swing no track field, an invalid id nothing.
    CHECK(core::perf::setStepsParam(0, 0, 0, 1, id("seq.transpose"), 3.0f) == nullptr);
    CHECK(core::perf::setStepsParam(0, 0, 0, 0, id("seq.swing"), 3.0f) == nullptr);
    CHECK(core::perf::setStepsParam(0, 0, 0, 0, core::ParamId {}, 3.0f) == nullptr);
    // Any other lockable parameter on selected steps (a pitch lock).
    REQUIRE(core::perf::setStepsParam(0, 1, 0, 2, id("playback.pitch"), 7.0f) != nullptr);
}
