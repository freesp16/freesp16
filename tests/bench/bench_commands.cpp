// Command bus budget: execute of a SetParam ≤ 5 µs on the host — apply, inverse, record,
// bridge hook, pending notification. Message thread, so the host number is the gate; the Cortex-A9 figure is open
// (emulator timings are relative only).
#include "core/commands/CommandBusAccess.h"
#include "core/commands/SetParam.h"
#include "core/commands/UndoStack.h"
#include "core/model/Project.h"
#include "tests/fakes/FakeClock.h"
#include "tests/bench/harness/Bench.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace
{
using core::model::ParamPath;
using core::model::Project;
using core::model::Scope;

int& volume(Project& p, const ParamPath& at)
{
    return p.scenes[at.scene].tracks[at.track].volume;
}

constexpr double kBudgetUs = 5.0;

// Paths built once, like a screen's cached strip bindings; the command still copies its path.
std::vector<std::string> paths()
{
    std::vector<std::string> out;
    for (int s = 0; s < 16; ++s)
        for (int t = 0; t < 16; ++t)
            out.push_back("scene[" + std::to_string(s) + "].track[" + std::to_string(t) + "].volume");
    return out;
}

template <typename Fn> double usPerOp(int n, Fn&& fn)
{
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i)
        fn(i);
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::micro>(t1 - t0).count() / n;
}
} // namespace

TEST_CASE("bench commands: execute of a SetParam", "[bench][commands]")
{
    const auto names = paths();
    core::CommandBus bus;
    core::commands::replaceProject(bus, std::make_unique<Project>());
    fakes::FakeClock clock;
    core::commands::setClock(bus, &clock);
    int bridged = 0;
    core::commands::setBridge(bus,
                              [&](const core::ChangeSet& c, const Project&) { bridged += static_cast<int>(c.size()); });
    auto sub = bus.observe("scene[", [](const core::ChangeSet&) {});
    constexpr int n = 200000;

    // An encoder turn: every execute coalesces into the top entry (EDT-U2).
    const double coalesced =
        usPerOp(n,
                [&](int i)
                {
                    const ParamPath at{Scope::Track, 3, 7, 0, 0, 0, {}};
                    bus.execute(core::makeSetParam<int>(names[3 * 16 + 7], "VOLUME", &volume, at, i & 127));
                    clock.advance(10);
                    if ((i & 63) == 0)
                        core::commands::flushNotifications(bus); // a message-loop iteration every 64 edits
                });
    CHECK(core::commands::history(bus).undoDepth() == 1);

    // Distinct edits: every execute records a new undo entry.
    const double recorded = usPerOp(
        n,
        [&](int i)
        {
            const int k = i & 255;
            const ParamPath at{
                Scope::Track, static_cast<std::uint8_t>(k >> 4), static_cast<std::uint8_t>(k & 15), 0, 0, 0, {}};
            bus.execute(core::makeSetParam<int>(names[static_cast<std::size_t>(k)], "VOLUME", &volume, at, i & 127));
            clock.advance(1500);
            if ((i & 63) == 0)
                core::commands::flushNotifications(bus);
        });
    CHECK(core::commands::history(bus).undoDepth() == n + 1);

    // Undo (execute of the inverse) of those entries.
    const double undone = usPerOp(n, [&](int) { bus.undo(); });

    std::printf("BENCH commands execute_setparam coalesced=%.3fus recorded=%.3fus undo=%.3fus budget=%.1fus "
                "history_bytes=%zu\n",
                coalesced, recorded, undone, kBudgetUs, core::commands::history(bus).bytes());
    bench::record("commands_execute_coalesced", bench::usToCycles(kBudgetUs), bench::usToCycles(coalesced));
    bench::record("commands_execute_recorded", bench::usToCycles(kBudgetUs), bench::usToCycles(recorded));
    bench::record("commands_undo", bench::usToCycles(kBudgetUs), bench::usToCycles(undone));
    CHECK(bridged == 3 * n);
    CHECK(coalesced <= kBudgetUs);
    CHECK(recorded <= kBudgetUs);
    CHECK(undone <= kBudgetUs);
}
