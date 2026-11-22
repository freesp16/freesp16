// Arrangement row commands and the plan builder (ARR-001…004, ARR-011/012, EDT-U1/U2): insert / append / paste /
// delete / move / duplicate / edit / clear each undo in one step and redo; knob edits coalesce per row field; the 999
// row cap and invalid indices refuse; song mode toggles undoably; the plan's start table follows the pattern lengths
// and is rebuilt for the right ChangeSet paths.
#include "core/commands/CommandBusAccess.h"
#include "core/commands/UndoStack.h"
#include "core/commands/arrangement/ArrangementCommands.h"
#include "core/commands/arrangement/ArrangementPlanBuilder.h"
#include "core/commands/scene/SceneCommands.h"
#include "core/model/ModelOps.h"
#include "core/params/ParamRegistry.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

namespace ar = core::commands::arrangement;
namespace m = core::model;

namespace
{
struct Doc
{
    core::CommandBus bus;
    std::vector<core::ChangeSet> changes;
    Doc()
    {
        core::commands::replaceProject(bus, m::newDefaultProject());
        core::commands::setBridge(bus, [this](const core::ChangeSet& c, const m::Project&) { changes.push_back(c); });
    }
    const m::Project& p() const { return core::commands::project(bus); }
    const std::vector<m::Section>& rows() const { return p().arrangement.sections; }
    bool run(std::unique_ptr<core::Command> c)
    {
        if (! c)
            return false;
        bus.execute(std::move(c));
        return true;
    }
    std::size_t steps() const { return core::commands::history(bus).undoDepth(); }
};

m::Section row(int scene, int pattern, int repeats = 1, double bpm = 0)
{
    m::Section s;
    s.scene = static_cast<std::uint8_t>(scene);
    s.pattern = static_cast<std::uint8_t>(pattern);
    s.repeats = repeats;
    if (bpm > 0)
        s.fixedBpm = bpm;
    return s;
}

std::vector<int> patterns(const Doc& d)
{
    std::vector<int> out;
    for (const auto& s : d.rows())
        out.push_back(s.scene * 16 + s.pattern);
    return out;
}

// Applies `c`, checks one new undo step, undo restores `before` exactly, redo gives the edited rows again.
void oneStep(Doc& d, std::unique_ptr<core::Command> c)
{
    const auto before = d.rows();
    const auto steps = d.steps();
    REQUIRE(d.run(std::move(c)));
    const auto after = d.rows();
    CHECK(d.steps() == steps + 1);
    CHECK(d.p().arrangement.length == static_cast<int>(after.size()));
    REQUIRE(d.bus.undo());
    CHECK(d.rows() == before);
    CHECK(d.p().arrangement.length == static_cast<int>(before.size()));
    REQUIRE(d.bus.redo());
    CHECK(d.rows() == after);
}
} // namespace

TEST_CASE("arr.commands: insert, append, paste, delete, move, duplicate, edit and clear each undo in one step",
          "[arrangement]")
{
    Doc d;
    oneStep(d, ar::appendRow(d.p(), row(0, 0, 2)));
    oneStep(d, ar::appendRow(d.p(), row(1, 3, 1, 140)));
    CHECK(patterns(d) == std::vector<int> {0, 19});
    // Selector push: the current scene / pattern before the focused row (UI-303), REPEAT 1, BPM "--".
    d.run(core::commands::scene::setCurrentScenePattern(2, 5));
    oneStep(d, ar::insertCurrentBefore(d.p(), 1));
    CHECK(patterns(d) == std::vector<int> {0, 37, 19});
    CHECK(d.rows()[1].repeats == 1);
    CHECK_FALSE(d.rows()[1].fixedBpm.has_value());
    // Insert before END OF ARRANGEMENT (row = count) appends.
    oneStep(d, ar::insertRowBefore(d.p(), 3, row(4, 4)));
    CHECK(patterns(d) == std::vector<int> {0, 37, 19, 68});
    // COPY + PASTE (always inserts before the focused row, ARR-012).
    ar::RowClipboard clip;
    CHECK_FALSE(clip.copy(d.p(), 4)); // END: nothing to copy
    CHECK(clip.pasteBefore(d.p(), 0) == nullptr);
    REQUIRE(clip.copy(d.p(), 2));
    oneStep(d, clip.pasteBefore(d.p(), 0));
    CHECK(patterns(d) == std::vector<int> {19, 0, 37, 19, 68});
    CHECK(d.rows()[0].fixedBpm == std::optional<double>(140.0));
    oneStep(d, ar::deleteRow(d.p(), 1));
    CHECK(patterns(d) == std::vector<int> {19, 37, 19, 68});
    oneStep(d, ar::moveRow(d.p(), 0, 3));
    CHECK(patterns(d) == std::vector<int> {37, 19, 68, 19});
    oneStep(d, ar::duplicateRow(d.p(), 1));
    CHECK(patterns(d) == std::vector<int> {37, 19, 19, 68, 19});
    oneStep(d, ar::editRow(d.p(), 0, row(15, 15, 99, 300)));
    CHECK(d.rows()[0].repeats == 99);
    oneStep(d, ar::clearArrangement(d.p()));
    CHECK(d.rows().empty());
    // Every edit reached the bridge with the plan's path.
    for (const auto& c : d.changes)
        if (! c.touches("settings"))
            CHECK(ar::affectsPlan(c));
}

TEST_CASE("arr.commands: knob edits of one row field coalesce; values clamp; no-ops and bad indices refuse",
          "[arrangement]")
{
    Doc d;
    d.run(ar::appendRow(d.p(), row(0, 0)));
    d.run(ar::appendRow(d.p(), row(0, 1)));
    const auto steps = d.steps();
    for (int bpm = 100; bpm <= 120; ++bpm)
        d.run(ar::editRowField(d.p(), 1, ar::RowField::Bpm, bpm));
    CHECK(d.steps() == steps + 1); // one undo step for the turn (EDT-U2)
    CHECK(d.rows()[1].fixedBpm == std::optional<double>(120.0));
    d.run(ar::editRowField(d.p(), 1, ar::RowField::Repeat, 7));
    CHECK(d.steps() == steps + 2); // another field: its own step
    REQUIRE(d.bus.undo());
    REQUIRE(d.bus.undo());
    CHECK_FALSE(d.rows()[1].fixedBpm.has_value());
    CHECK(d.rows()[1].repeats == 1);
    // Clamping: repeats 1–99, BPM below 20 = "--", above 300 = 300 on the 0.1 grid, scene / pattern 0–15.
    d.run(ar::editRowField(d.p(), 0, ar::RowField::Repeat, 500));
    CHECK(d.rows()[0].repeats == 99);
    d.run(ar::editRowField(d.p(), 0, ar::RowField::Bpm, 12.0));
    CHECK_FALSE(d.rows()[0].fixedBpm.has_value());
    d.run(ar::editRowField(d.p(), 0, ar::RowField::Bpm, 123.456));
    CHECK(d.rows()[0].fixedBpm == std::optional<double>(123.5));
    d.run(ar::editRowField(d.p(), 0, ar::RowField::Scene, 40));
    CHECK(d.rows()[0].scene == 15);
    // The knob values of the focused row (UI-301); END shows the defaults.
    CHECK(ar::rowValue(d.p(), 0, ar::RowField::Scene) == 15.0f);
    CHECK(ar::rowValue(d.p(), 0, ar::RowField::Bpm) == 123.5f);
    CHECK(ar::rowValue(d.p(), 2, ar::RowField::Repeat) == 1.0f);
    // Refusals.
    CHECK(ar::editRowField(d.p(), 0, ar::RowField::Scene, 15) == nullptr); // nothing changes
    CHECK(ar::deleteRow(d.p(), 2) == nullptr);                             // END
    CHECK(ar::moveRow(d.p(), 0, 0) == nullptr);
    CHECK(ar::moveRow(d.p(), 0, 5) == nullptr);
    CHECK(ar::insertRowBefore(d.p(), 3, row(0, 0)) == nullptr);
    CHECK(ar::editRow(d.p(), -1, row(0, 0)) == nullptr);
    // The arrangement.row.* ParamSpecs exist with the knob ranges.
    const auto& reg = core::builtinParams();
    for (const char* id : {"arrangement.row.scene", "arrangement.row.pattern", "arrangement.row.repeat",
                           "arrangement.row.bpm", "arrangement.play", "arrangement.loop", "arrangement.start"})
        CHECK(reg.find(id) != nullptr);
    char buf[16];
    reg.find("arrangement.row.bpm")->formatValue(0.0f, buf, sizeof buf);
    CHECK(std::string(buf) == "--");
    reg.find("arrangement.row.scene")->formatValue(3.0f, buf, sizeof buf);
    CHECK(std::string(buf) == "4");
}

TEST_CASE("arr.commands: 999 rows at most; song mode toggles undoably", "[arrangement]")
{
    Doc d;
    std::vector<m::Section> many(998, row(0, 0));
    d.bus.execute(ar::appendRow(d.p(), row(1, 1)));
    core::commands::project(d.bus).arrangement.sections.insert(core::commands::project(d.bus).arrangement.sections.end(),
                                                               many.begin(), many.end());
    REQUIRE(ar::rowCount(d.p()) == 999);
    CHECK(ar::appendRow(d.p(), row(0, 0)) == nullptr);
    CHECK(ar::insertCurrentBefore(d.p(), 0) == nullptr);
    CHECK(ar::duplicateRow(d.p(), 0) == nullptr);
    ar::RowClipboard clip;
    REQUIRE(clip.copy(d.p(), 0));
    CHECK(clip.pasteBefore(d.p(), 0) == nullptr);
    REQUIRE(d.run(ar::deleteRow(d.p(), 998)));
    CHECK(d.run(clip.pasteBefore(d.p(), 0)));
    CHECK(ar::rowCount(d.p()) == 999);

    d.changes.clear();
    REQUIRE(d.run(ar::setPlayArrangement(d.p(), true)));
    CHECK(d.p().settings.playArrangement);
    CHECK(ar::setPlayArrangement(d.p(), true) == nullptr);
    REQUIRE(d.changes.size() == 1);
    CHECK(ar::affectsPlay(d.changes[0]));
    CHECK_FALSE(ar::affectsPlan(d.changes[0]));
    REQUIRE(d.bus.undo());
    CHECK_FALSE(d.p().settings.playArrangement);
}

TEST_CASE("arr.plan: the start table follows pattern lengths and repeats; rowAt finds rows", "[arrangement]")
{
    Doc d;
    d.run(core::commands::scene::setPatternLength(0, 1, 8));
    d.run(core::commands::scene::setPatternLength(1, 0, 64));
    d.run(ar::appendRow(d.p(), row(0, 0, 2)));      // 16 × 2
    d.run(ar::appendRow(d.p(), row(0, 1, 3, 140))); // 8 × 3
    d.run(ar::appendRow(d.p(), row(1, 0, 1)));      // 64
    auto plan = std::make_unique<core::arrangement::ArrangementPlan>();
    core::arrangement::SongStatus status;
    plan->status = &status;
    ar::buildPlan(d.p(), *plan);
    REQUIRE(plan->count == 3);
    CHECK(plan->status == &status);
    CHECK(plan->startPpq16[0] == 0);
    CHECK(plan->startPpq16[1] == 32);
    CHECK(plan->startPpq16[2] == 56);
    CHECK(plan->startPpq16[3] == 120);
    CHECK(plan->rows[1].bpm == 140.0f);
    CHECK(plan->rows[0].bpm == 0.0f);
    CHECK(plan->passLength16(1) == 8);
    CHECK(plan->rowAt(0) == 0);
    CHECK(plan->rowAt(31) == 0);
    CHECK(plan->rowAt(32) == 1);
    CHECK(plan->rowAt(119) == 2);
    CHECK(plan->rowAt(120) == -1);
    // The paths that rebuild the plan.
    auto touches = [](const char* path)
    {
        core::ChangeSet c;
        c.add(path);
        return ar::affectsPlan(c);
    };
    CHECK(touches("arrangement.sections"));
    CHECK(touches("scene[3]"));
    CHECK(touches("scene[3].pattern[12]"));
    CHECK(touches("scene[3].pattern[12].length"));
    CHECK_FALSE(touches("scene[3].pattern[12].step[4].track[2].velocity"));
    CHECK_FALSE(touches("scene[3].bpm"));
    CHECK_FALSE(touches("arrangement.play"));
    CHECK_FALSE(touches("settings.tempo"));
}
