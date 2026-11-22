// core::commands::ribbon (RIB-002/004/009/011, FX-095/096): assign / edit / delete USER items and the pitch
// range, as undoable commands on the project's RibbonSettings.
#include "core/commands/CommandBusAccess.h"
#include "core/commands/ribbon/RibbonCommands.h"
#include "core/model/ModelOps.h"
#include "core/params/ParamRegistry.h"

#include <catch2/catch_test_macros.hpp>

namespace rb = core::commands::ribbon;
namespace m = core::model;

namespace
{
struct Bus
{
    core::CommandBus bus;
    std::vector<std::string> paths;
    Bus()
    {
        core::commands::replaceProject(bus, m::newDefaultProject());
        core::commands::setBridge(bus, [this](const core::ChangeSet& c, const m::Project&)
                                  { paths.insert(paths.end(), c.paths().begin(), c.paths().end()); });
    }
    const m::RibbonSettings& ribbon() { return core::commands::project(bus).ribbon; }
    const m::Project& project() { return core::commands::project(bus); }
    bool run(std::unique_ptr<core::Command> c)
    {
        if (! c)
            return false;
        bus.execute(std::move(c));
        return true;
    }
};
} // namespace

TEST_CASE("ribbon.commands: assign appends to the list, edits the range, deletes and compacts; undo restores",
          "[ribbon][commands]")
{
    Bus b;
    CHECK(rb::itemCount(b.ribbon(), 0) == 0);
    CHECK(b.run(rb::assignItem(b.project(), 0, 5, "playback.pitch"))); // past the end: appended at slot 0
    CHECK(b.ribbon().user[0][0].target == "playback.pitch");
    CHECK(b.ribbon().user[0][0].bottom == 0.0);
    CHECK(b.ribbon().user[0][0].top == 1.0);
    CHECK(b.run(rb::assignItem(b.project(), 0, 1, "track.volume")));
    CHECK(b.run(rb::assignItem(b.project(), 0, 2, "fx2.gainTest.gain"))); // FX-095: an insert-FX knob on FX2
    CHECK(rb::itemCount(b.ribbon(), 0) == 3);
    CHECK(b.paths.back() == "ribbon");

    CHECK_FALSE(b.run(rb::assignItem(b.project(), 0, 3, "transport.tempo"))); // not a strip target
    CHECK_FALSE(b.run(rb::assignItem(b.project(), 0, 3, "no.such.param")));
    CHECK_FALSE(b.run(rb::assignItem(b.project(), 2, 0, "track.pan")));      // no USER3
    CHECK_FALSE(b.run(rb::setItemRange(b.project(), 0, 6, 0.2, 0.8)));       // empty slot

    CHECK(b.run(rb::setItemRange(b.project(), 0, 1, 0.75, 0.25)));
    CHECK(b.run(rb::setItemRange(b.project(), 0, 1, 1.5, -1.0))); // clamped
    CHECK(b.ribbon().user[0][1].bottom == 1.0);
    CHECK(b.ribbon().user[0][1].top == 0.0);
    CHECK(b.bus.undo()); // the two knob edits of one item coalesce into one undo step (EDT-U2)
    CHECK(b.ribbon().user[0][1].bottom == 0.0);
    CHECK(b.ribbon().user[0][1].top == 1.0);

    CHECK(b.run(rb::deleteItem(b.project(), 0, 0))); // RIB-011: the rest moves up
    CHECK(b.ribbon().user[0][0].target == "track.volume");
    CHECK(b.ribbon().user[0][1].target == "fx2.gainTest.gain");
    CHECK(b.ribbon().user[0][2].target.empty());
    CHECK(rb::itemCount(b.ribbon(), 0) == 2);
    CHECK_FALSE(b.run(rb::deleteItem(b.project(), 0, 5)));
    CHECK(b.bus.undo());
    CHECK(b.ribbon().user[0][0].target == "playback.pitch");
    CHECK(rb::itemCount(b.ribbon(), 0) == 3);
    CHECK(rb::itemCount(b.ribbon(), 1) == 0); // USER2 untouched

    for (int i = 0; i < 8; ++i) // a full list refuses the ninth
        b.run(rb::assignItem(b.project(), 1, i, i % 2 ? "track.pan" : "track.volume"));
    CHECK(rb::itemCount(b.ribbon(), 1) == 8);
}

TEST_CASE("ribbon.commands: the pitch range (RIB-002) is clamped and undoable", "[ribbon][commands]")
{
    Bus b;
    CHECK(b.ribbon().pitchTopCents == 200); // the project default (M p.15)
    CHECK(b.ribbon().pitchBottomCents == -200);
    CHECK(b.run(rb::setPitchRange(b.project(), -1200, 700)));
    CHECK(b.run(rb::setPitchRange(b.project(), -5000, 5000)));
    CHECK(b.ribbon().pitchBottomCents == -2400);
    CHECK(b.ribbon().pitchTopCents == 2400);
    CHECK_FALSE(b.run(rb::setPitchRange(b.project(), -2400, 2400)));
    CHECK(b.bus.undo()); // coalesced
    CHECK(b.ribbon().pitchTopCents == 200);
}

TEST_CASE("ribbon.commands: targets are the registry's strip targets; names per FX-096", "[ribbon][commands]")
{
    const auto& r = core::builtinParams();
    const auto targets = r.stripTargets();
    REQUIRE_FALSE(targets.empty());
    for (const auto id : targets)
    {
        const auto& spec = r[id];
        INFO(spec.id);
        CHECK(rb::isStripTarget(r, spec.id));
        const auto name = rb::targetName(spec);
        CHECK_FALSE(name.empty());
        CHECK(name.find("MIXRATIO") == std::string::npos);
        // FX-095: insert FX only (Send / Master FX parameters are not offered)
        CHECK(spec.id.substr(0, 5) != "send.");
        CHECK(spec.id.substr(0, 7) != "master.");
    }
    core::ParamSpec mix;
    mix.id = "fx1.chorus.mix";
    mix.label = mix.shortLabel = "MIXRATIO";
    CHECK(rb::targetName(mix) == "FX1 MIX");
    CHECK(rb::targetName(*r.find("playback.pitch")) == "PITCH");
}
