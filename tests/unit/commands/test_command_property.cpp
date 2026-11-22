// Property test: random command sequences — value edits of several types, step
// cells, sample assignments, groups, recording passes, interleaved undo/redo, coalescing at random intervals, saves —
// then undo all → the initial model and redo all → the final model, for 500 seeds. Whenever the bus says "clean",
// the model equals the one last saved.
#include "CommandTestFields.h"
#include "core/commands/CommandBusAccess.h"
#include "core/commands/CommandGroup.h"
#include "core/commands/UndoStack.h"
#include "tests/fakes/FakeClock.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <random>
#include <string>

namespace
{
using namespace cmdtest;
namespace cmd = core::commands;

struct Gen
{
    std::mt19937 rng;
    int pick(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); }

    std::unique_ptr<core::Command> edit()
    {
        // Few distinct targets, so coalescing and same-field interactions happen often.
        const int scene = pick(0, 1), track = pick(0, 2);
        const ParamPath at{
            Scope::Track, static_cast<std::uint8_t>(scene), static_cast<std::uint8_t>(track), 0, 0, 0, {}};
        switch (pick(0, 8))
        {
        case 0:
            return setVolume(scene, track, pick(0, 127));
        case 1:
            return setPan(scene, track, pick(-64, 64));
        case 2:
            return setTempo(60.0 + pick(0, 1200) / 10.0);
        case 3:
            return core::makeSetParam<double>(trackPath(scene, track, "ampEnv.attack"), "ATTACK", &envAttack, at,
                                              pick(0, 100) / 100.0);
        case 4:
            return core::makeSetParam<bool>(trackPath(scene, track, "mute"), "MUTE", &trackMute, at, pick(0, 1) == 1);
        case 5:
            return core::makeSetParam<std::string>(
                trackPath(scene, track, "name"), "NAME", &trackName, at,
                std::string(static_cast<std::size_t>(pick(0, 40)), static_cast<char>('a' + pick(0, 25))));
        case 6:
        {
            core::model::Step s;
            if (pick(0, 3) != 0)
            {
                s.trigger = static_cast<std::int16_t>(pick(0, 4));
                s.value2 = static_cast<std::int16_t>(pick(1, 127));
            }
            return setStep(scene, pick(0, 1), pick(0, 7), track, s);
        }
        case 7:
        {
            core::model::SampleRef ref;
            if (pick(0, 4) != 0)
                ref = sampleRef("s" + std::to_string(pick(0, 5)));
            return std::make_unique<core::AssignSample>(static_cast<std::uint8_t>(scene),
                                                        static_cast<std::uint8_t>(track), ref);
        }
        default:
        {
            auto g = std::make_unique<core::CommandGroup>("PASTE");
            const int n = pick(1, 6);
            for (int i = 0; i < n; ++i)
                g->add(pick(0, 5) == 0 ? edit() : setVolume(scene, pick(0, 2), pick(0, 127)));
            return g;
        }
        }
    }
};

void runSeed(std::uint32_t seed)
{
    Gen gen{std::mt19937(seed)};
    core::CommandBus bus;
    giveProject(bus);
    fakes::FakeClock clock;
    cmd::setClock(bus, &clock);
    int bridged = 0;
    cmd::setBridge(bus, [&](const core::ChangeSet& c, const Project&) { bridged += c.empty() ? 1000 : 1; });

    const auto initial = std::make_unique<Project>(cmd::project(bus)); // ≈ 2 MB: heap
    auto saved = std::make_unique<Project>(*initial);
    auto checkClean = [&]
    {
        if (! bus.dirty())
            REQUIRE(cmd::project(bus) == *saved);
    };

    const int ops = gen.pick(20, 120);
    for (int i = 0; i < ops; ++i)
    {
        clock.advance(gen.pick(0, 3) == 0 ? 1500.0 : gen.pick(0, 400));
        const int what = gen.pick(0, 99);
        if (what < 60)
            bus.execute(gen.edit());
        else if (what < 72)
            bus.undo();
        else if (what < 80)
            bus.redo();
        else if (what < 85)
            cmd::beginGroup(bus);
        else if (what < 90)
            cmd::endGroup(bus);
        else if (what < 94)
        {
            bus.checkpoint();
            *saved = cmd::project(bus);
        }
        else
            cmd::flushNotifications(bus);
        checkClean();
    }
    cmd::endGroup(bus);
    const auto final = std::make_unique<Project>(cmd::project(bus));

    int undone = 0;
    while (bus.undo())
    {
        ++undone;
        checkClean();
    }
    REQUIRE(cmd::project(bus) == *initial);
    CHECK(cmd::history(bus).evicted() == 0);
    for (int i = 0; i < undone; ++i)
    {
        REQUIRE(bus.redo());
        checkClean();
    }
    REQUIRE(cmd::project(bus) == *final);
    CHECK(bridged < 1000); // every bridge call carried the paths it touched
}
} // namespace

TEST_CASE("command property: undo all restores the initial model, redo all the final (500 seeds)", "[commands][undo]")
{
    for (std::uint32_t seed = 1; seed <= 500; ++seed)
    {
        CAPTURE(seed);
        runSeed(seed);
    }
}
