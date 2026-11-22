// Sampling commands (SMP-009/010/025, EDT-U5): assign take ("RECORDED TEMP."), save & replace,
// discard (the replaced reference and region come back), unload — each one undo step on the CommandBus.
#include "core/commands/CommandBusAccess.h"
#include "core/commands/UndoStack.h"
#include "core/commands/sampling/SamplingCommands.h"
#include "core/model/ModelOps.h"
#include "core/model/Project.h"

#include <catch2/catch_test_macros.hpp>

namespace sc = core::commands::sampling;
using core::model::SampleRef;

namespace
{
core::model::Track& track(core::CommandBus& bus, int s, int t)
{
    return core::commands::project(bus).scenes[static_cast<std::size_t>(s)].tracks[static_cast<std::size_t>(t)];
}
} // namespace

TEST_CASE("sampling.commands: a take replaces the track as RECORDED TEMP.; discard restores the old sample",
          "[sampling][commands]")
{
    core::CommandBus bus;
    core::commands::replaceProject(bus, core::model::newDefaultProject());
    const SampleRef kick {"sha256:k", "$Lib/Samples/kick.wav", "kick", false, {}};
    bus.execute(core::commands::library::loadSample(2, 7, kick, {30000, false}));
    auto& t = track(bus, 2, 7);
    t.playback.start = 100;
    t.playback.length = 2000;
    const auto replaced = sc::replacedOf(core::commands::project(bus), 2, 7);
    REQUIRE(replaced.ref.has_value());
    CHECK(replaced.ref->id == "sha256:k");
    CHECK(replaced.start == 100);

    const SampleRef take {"sha256:t", "$Lib/Recordings/REC 001.wav", "REC 001.wav", false, {}};
    bus.execute(sc::assignTake(2, 7, take, {176400, true}));
    CHECK(track(bus, 2, 7).playback.sample.id == "sha256:t");
    CHECK(track(bus, 2, 7).playback.length == 176400);
    CHECK(sc::isRecordedTemp(core::commands::project(bus), 2, 7));
    CHECK(core::commands::history(bus).undoLabel() == "RECORD TAKE");

    bus.execute(sc::discardTake(2, 7, replaced));
    CHECK(track(bus, 2, 7).playback.sample.id == "sha256:k");
    CHECK(track(bus, 2, 7).playback.start == 100);
    CHECK(track(bus, 2, 7).playback.length == 2000);
    CHECK_FALSE(sc::isRecordedTemp(core::commands::project(bus), 2, 7));
    REQUIRE(bus.undo()); // discard is undoable: the take is back
    CHECK(track(bus, 2, 7).playback.sample.id == "sha256:t");
}

TEST_CASE("sampling.commands: save & replace assigns the saved file; unload clears the track; discard on an empty track",
          "[sampling][commands]")
{
    core::CommandBus bus;
    core::commands::replaceProject(bus, core::model::newDefaultProject());
    const auto empty = sc::replacedOf(core::commands::project(bus), 0, 0);
    CHECK_FALSE(empty.ref.has_value());
    const SampleRef take {"sha256:t", "$Lib/Recordings/REC 001.wav", "REC 001.wav", false, {}};
    bus.execute(sc::assignTake(0, 0, take, {1000, false}));
    const SampleRef saved {"sha256:s", "$Lib/Samples/REC 001 cut.wav", "REC 001 cut.wav", false, {}};
    bus.execute(sc::saveAndReplace(0, 0, saved, {500, false}));
    CHECK(track(bus, 0, 0).playback.sample.id == "sha256:s");
    CHECK_FALSE(sc::isRecordedTemp(core::commands::project(bus), 0, 0));
    CHECK(core::commands::history(bus).undoLabel() == "SAVE & REPLACE");
    bus.execute(sc::unloadTrack(0, 0));
    CHECK(track(bus, 0, 0).playback.sample.empty());
    REQUIRE(bus.undo());
    CHECK(track(bus, 0, 0).playback.sample.id == "sha256:s");

    bus.execute(sc::assignTake(0, 1, take, {1000, false}));
    bus.execute(sc::discardTake(0, 1, sc::replacedOf(*core::model::newDefaultProject(), 0, 1)));
    CHECK(track(bus, 0, 1).playback.sample.empty());
}
