// The panel wiring of the edits: the chain gesture on the PATTERN keys (SCN-030: hold the current pattern + press
// others, the chain goes to the sink on release; the current key alone still cancels; a plain selection clears the
// chain) and the COPY / PASTE step gesture on the step controller (EDT-005: the step keys copy / paste instead of
// entering triggers).
#include "tests/unit/edit/EditTestUtil.h"

#include "core/commands/scene/SceneCommands.h"
#include "core/edit/SharedEditor.h"
#include "ui/input/scenes/ScenePatternKeys.h"
#include "ui/input/steps/StepController.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace ce = core::edit;
namespace m = core::model;
namespace sc = core::commands::scene;
using edittest::cell;

namespace
{
io::ControlEvent ev(io::ControlEvent::Kind k, io::Control c)
{
    return io::ControlEvent {k, c, 127, 0, 0.0};
}
void tap(ui::input::scenes::ScenePatternKeys& keys, io::Control c)
{
    keys.handle(ev(io::ControlEvent::Kind::Press, c), false);
    keys.handle(ev(io::ControlEvent::Kind::Release, c), false);
}
} // namespace

TEST_CASE("edit.wiring: hold the current pattern key and press others: the chain 1→2→2→4 reaches the sink", "[edit]")
{
    edittest::Bus b(false);
    ui::input::scenes::ScenePatternKeys keys(b.bus);
    std::vector<core::chain::ChainPlan> posted;
    keys.setChainSink([&posted](const core::chain::ChainPlan& p) { posted.push_back(p); });
    std::vector<int> queued;
    auto sub = b.bus.observe("queue[", [&queued](const core::ChangeSet& c) {
        for (const auto& p : c.paths())
            if (auto v = sc::queueValueOf(p))
                queued.push_back(*v);
    });
    tap(keys, io::Control::Pattern); // PATTERN mode; current = pattern 1
    using K = io::ControlEvent::Kind;
    keys.handle(ev(K::Press, io::step(1)), false);
    CHECK(keys.chainHeld());
    for (int k : {2, 2, 4})
        tap(keys, io::step(k));
    keys.handle(ev(K::Release, io::step(1)), false);
    core::commands::flushNotifications(b.bus);
    REQUIRE(posted.size() == 1);
    CHECK(posted[0].length == 4);
    CHECK(std::vector<int>(posted[0].patterns.begin(), posted[0].patterns.begin() + 4) == std::vector<int> {0, 1, 1, 3});
    CHECK(queued.empty()); // building a chain selects nothing
    // The current key alone: the plain press (SCN-021 cancel) and the chain is cleared.
    tap(keys, io::step(1));
    core::commands::flushNotifications(b.bus);
    REQUIRE(posted.size() == 2);
    CHECK(posted[1].length == 0);
    CHECK(queued == std::vector<int> {0});
    // A plain selection of another pattern: selected at once, no chain to clear.
    tap(keys, io::step(3));
    core::commands::flushNotifications(b.bus);
    CHECK(posted.size() == 2);
    CHECK(queued.back() == 2);
    // Without a sink the current key acts on its press, as before.
    ui::input::scenes::ScenePatternKeys plain(b.bus);
    tap(plain, io::Control::Pattern);
    plain.handle(ev(K::Press, io::step(1)), false);
    CHECK_FALSE(plain.chainHeld());
}

TEST_CASE("edit.wiring: ext.copy + step keys and ext.paste + a step key on the step controller (EDT-005)", "[edit]")
{
    ce::resetSharedEditor();
    edittest::Bus b(false);
    auto& pat = b.project().scenes[0].patterns[0];
    edittest::putStep(pat, 0, 2, 100, 4, 2.0f);
    edittest::putStep(pat, 4, 2, 90);
    ui::input::steps::setShownBar(0);
    ui::input::steps::StepController steps(b.bus, nullptr);
    steps.setContext(0, 0, 2);
    steps.setCopy(true);
    CHECK(steps.copyGestureActive());
    steps.onStepKey(1, true, 0);
    steps.onStepKey(1, false, 0);
    steps.onStepKey(5, true, 0);
    steps.onStepKey(5, false, 0);
    steps.setCopy(false);
    CHECK_FALSE(steps.copyGestureActive());
    CHECK(b.undoDepth() == 0); // copying toggled nothing
    steps.setContext(0, 3, 2); // pattern 4
    steps.setPaste(true);
    steps.onStepKey(3, true, 0);
    steps.onStepKey(3, false, 0);
    steps.setPaste(false);
    CHECK(b.undoDepth() == 1); // the paste (inline workers here)
    CHECK(cell(b.pattern(0, 3), 2, 2) == cell(pat, 0, 2));
    CHECK(cell(b.pattern(0, 3), 6, 2) == cell(pat, 4, 2));
    // Afterwards the keys enter triggers again.
    steps.onStepKey(10, true, 0);
    steps.onStepKey(10, false, 0);
    CHECK(b.pattern(0, 3).steps[9][2].trigger != m::Step::kEmpty);
    ce::resetSharedEditor();
}
