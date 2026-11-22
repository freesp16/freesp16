// Strip moves recorded as p-locks (RIB-008; ui/input/ext/striprecord/StripRecorder.h): USER
// items lock their targets, PITCH locks playback.pitch, only while recording and touching, once per step and value;
// non-lockable targets (Send FX, FX-094) and REPEAT are not recorded; the writes land as step locks in the pattern.
#include "core/commands/CommandBusAccess.h"
#include "core/commands/sequencer/SequencerCommands.h"
#include "core/model/ModelOps.h"
#include "core/params/ParamRegistry.h"
#include "core/util/ribbon/Ribbon.h"
#include "ui/input/ext/striprecord/StripRecorder.h"

#include <catch2/catch_test_macros.hpp>

namespace ext = ui::input::ext;
namespace m = core::model;

namespace
{
m::RibbonSettings ribbon()
{
    m::RibbonSettings r;
    r.user[0][0] = {"track.pan", 1.0, 0.0};      // full range, bottom → top
    r.user[0][1] = {"playback.pitch", 0.75, 0.25};
    for (const auto& s : core::builtinParams().all())
        if (s.id.substr(0, 5) == "send." && s.type == core::ParamType::Continuous)
        {
            r.user[0][2] = {std::string(s.id), 1.0, 0.0}; // not lockable (FX-094): skipped
            break;
        }
    r.pitchTopCents = 1200;
    r.pitchBottomCents = -1200;
    return r;
}

ext::StripRecordState state(int step, int mode, int position, std::uint16_t tracks = 1u << 2)
{
    ext::StripRecordState s;
    s.recording = true;
    s.scene = 1;
    s.pattern = 3;
    s.step = step;
    s.mode = mode;
    s.contact = true;
    s.position = position;
    s.tracks = tracks;
    return s;
}

int slotOf(const char* id) { return core::commands::sequencer::lockSlotOf(*core::builtinParams().find(id)); }
} // namespace

TEST_CASE("ext striprecord: USER moves lock their targets on the playing step, once per step and value",
          "[ext][striprecord]")
{
    ext::StripRecorder rec;
    const auto rib = ribbon();
    std::vector<ext::LockWrite> out;
    rec.frame(state(4, 2, 127), rib, out); // USER1, top
    REQUIRE(out.size() == 2);               // pan + pitch; the Send FX item is skipped
    CHECK(out[0].scene == 1);
    CHECK(out[0].pattern == 3);
    CHECK(out[0].track == 2);
    CHECK(out[0].step == 4);
    CHECK(out[0].slot == slotOf("track.pan"));
    CHECK(out[0].value == core::builtinParams().find("track.pan")->maxValue);
    CHECK(out[1].slot == slotOf("playback.pitch"));
    const auto* pitch = core::builtinParams().find("playback.pitch");
    CHECK(out[1].value == pitch->minValue + 0.75f * (pitch->maxValue - pitch->minValue));
    out.clear();
    rec.frame(state(4, 2, 127), rib, out); // same step, same value: nothing
    CHECK(out.empty());
    rec.frame(state(5, 2, 127), rib, out); // the next step: written again
    CHECK(out.size() == 2);
    out.clear();
    rec.frame(state(5, 2, 0), rib, out); // moved on the same step
    CHECK(out.size() == 2);
    CHECK(out[0].value == core::builtinParams().find("track.pan")->minValue);
}

TEST_CASE("ext striprecord: PITCH moves are pitch locks; REPEAT, no touch or no recording write nothing",
          "[ext][striprecord]")
{
    ext::StripRecorder rec;
    const auto rib = ribbon();
    std::vector<ext::LockWrite> out;
    rec.frame(state(0, 0, 127), rib, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].slot == slotOf("playback.pitch"));
    CHECK(out[0].value == 12.0f); // +1200 cents at the top
    out.clear();
    rec.frame(state(1, 0, 64), rib, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].value == 0.0f); // the centre: no bend
    out.clear();
    rec.frame(state(2, 1, 100), rib, out); // REPEAT
    CHECK(out.empty());
    auto s = state(3, 0, 100);
    s.recording = false;
    rec.frame(s, rib, out);
    s = state(3, 0, 100);
    s.contact = false;
    rec.frame(s, rib, out);
    s = state(3, 0, 100, 0);
    rec.frame(s, rib, out);
    CHECK(out.empty());
    // Two tracks held: one lock each.
    rec.frame(state(6, 0, 0, (1u << 0) | (1u << 9)), rib, out);
    REQUIRE(out.size() == 2);
    CHECK(out[0].track == 0);
    CHECK(out[1].track == 9);
    CHECK(out[0].value == -12.0f);
}

TEST_CASE("ext striprecord: the writes are step locks of the pattern (SetLock commands)", "[ext][striprecord]")
{
    core::CommandBus bus;
    core::commands::replaceProject(bus, m::newDefaultProject());
    ext::StripRecorder rec;
    std::vector<ext::LockWrite> out;
    rec.frame(state(7, 0, 127), ribbon(), out);
    for (const auto& w : out)
        bus.execute(core::commands::sequencer::setLock(w.scene, w.pattern, w.track, w.step, w.slot, w.value));
    const auto& pat = core::commands::project(bus).scenes[1].patterns[3];
    const auto& cell = pat.steps[7][2];
    REQUIRE(cell.lockIndex != m::Step::kNoLocks);
    const auto& l = pat.locks[cell.lockIndex];
    const int slot = slotOf("playback.pitch");
    CHECK((l.mask >> slot & 1u) != 0);
    CHECK(l.values[static_cast<std::size_t>(slot)] == 12.0f);
}
