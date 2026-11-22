// Steps and pages (EDT-005/006): placement relative to the target step, wrapping at the target pattern length when the
// copied span fits, positions kept (not played) when it does not; lock records travel with their cells and the target's
// records stay compact; the change paths the bridge needs.
#include "tests/unit/edit/EditTestUtil.h"

#include "core/edit/EditOps.h"
#include "core/edit/SceneEditor.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

namespace ce = core::edit;
namespace m = core::model;
using edittest::cell;

TEST_CASE("edit.steps: placement wraps at the target length when the span fits (EDT-005), else keeps it (EDT-006)",
          "[edit]")
{
    const std::uint64_t four = 0b1111ull << 2; // steps 2…5
    CHECK(ce::placeStep(2, four, 14, 16) == 14);
    CHECK(ce::placeStep(3, four, 14, 16) == 15);
    CHECK(ce::placeStep(4, four, 14, 16) == 0); // wraps at 16
    CHECK(ce::placeStep(5, four, 14, 16) == 1);
    const std::uint64_t all = ~0ull; // 64 steps into a 16-step pattern: positions kept
    CHECK(ce::placeStep(40, all, 0, 16) == 40);
    CHECK(ce::placeStep(63, all, 0, 16) == 63);
    CHECK(ce::placeStep(10, all, 8, 16) == 18);
    CHECK(ce::placeStep(63, all, 8, 16) == 7); // past the matrix: mod 64
    CHECK(ce::lowestStep(four) == 2);
    CHECK(ce::highestStep(four) == 5);
    CHECK(ce::lowestStep(0) == 0);
}

TEST_CASE("edit.steps: hold COPY + steps, hold PASTE + target step: wraps at a 16-step target", "[edit]")
{
    edittest::Bus b(false);
    auto& src = b.project().scenes[0].patterns[0];
    edittest::putStep(src, 2, 3, 90, 5, 1.5f);
    edittest::putStep(src, 3, 3, 91);
    edittest::putStep(src, 5, 3, 92, 7, 2.5f);
    b.project().scenes[0].patterns[1].length = 16;
    auto& dst0 = b.project().scenes[0].patterns[1];
    edittest::putStep(dst0, 0, 3, 10, 1, 9.0f); // overwritten by the wrap
    edittest::putStep(dst0, 8, 3, 11, 2, 8.0f); // stays
    ce::InlineWorkerPool pool;
    ce::SceneEditor editor(b.bus, pool);
    editor.copy(ce::stepsRef(0, 0, 3, (1ull << 2) | (1ull << 3) | (1ull << 4) | (1ull << 5)));
    editor.paste(ce::stepsRef(0, 1, 3, 1ull << 14));
    const auto& dst = b.pattern(0, 1);
    CHECK(cell(dst, 14, 3) == cell(src, 2, 3));
    CHECK(cell(dst, 15, 3) == cell(src, 3, 3));
    CHECK(cell(dst, 0, 3) == cell(src, 4, 3)); // the empty source step clears the target
    CHECK(dst.steps[0][3].empty());
    CHECK(cell(dst, 1, 3) == cell(src, 5, 3));
    CHECK(dst.locks.size() == 3); // compact: 14, 1 and 8 (the overwritten record of step 0 is gone)
    CHECK(cell(dst, 8, 3).hasLocks);
    CHECK(cell(dst, 8, 3).locks.values[2] == 8.0f);
    CHECK(cell(dst, 14, 3).locks.values[5] == 1.5f);
    CHECK(cell(dst, 1, 3).locks.values[7] == 2.5f);
    std::vector<std::string> problems;
    CHECK(m::validate(b.project(), problems));
}

TEST_CASE("edit.steps: compactLocks orders records by first use and drops unreferenced ones", "[edit]")
{
    m::Pattern p = m::defaultPattern();
    edittest::putStep(p, 10, 0, 1, 1, 1.0f);
    edittest::putStep(p, 2, 0, 1, 2, 2.0f);
    p.locks.push_back({}); // orphan
    p.steps[20][4].lockIndex = 999; // out of range
    ce::compactLocks(p);
    REQUIRE(p.locks.size() == 2);
    CHECK(p.steps[2][0].lockIndex == 0);
    CHECK(p.steps[10][0].lockIndex == 1);
    CHECK(p.locks[0].values[2] == 2.0f);
    CHECK(p.steps[20][4].lockIndex == m::Step::kNoLocks);
    const auto again = p;
    ce::compactLocks(p);
    CHECK(p == again);
}

TEST_CASE("edit.steps: the change paths name the slots and expand tracks into their parameters", "[edit]")
{
    core::ChangeSet c;
    ce::addTrackPaths(c, 2, 7);
    CHECK(c.touches("scene[2].track[7].volume"));
    CHECK(c.touches("scene[2].track[7].mute"));
    CHECK(c.touches("scene[2].track[7].insertFx[0]"));
    CHECK(c.touches("scene[2].track[7].insertFx[1]"));
    CHECK(std::none_of(c.paths().begin(), c.paths().end(), [](const std::string& p) { return p == "scene[2].track[7]"; }));
    CHECK(std::none_of(c.paths().begin(), c.paths().end(),
                       [](const std::string& p) { return p.find("queue") != std::string::npos; }));
    core::ChangeSet s;
    ce::addScenePaths(s, 4);
    CHECK(std::none_of(s.paths().begin(), s.paths().end(), [](const std::string& p) { return p == "scene[4]"; }));
    CHECK(s.touches("scene[4].bpm"));
    CHECK(s.touches("scene[4].pattern[15]"));
    CHECK(s.touches("scene[4].sendFx"));
    CHECK(s.touches("scene[4].track[15].volume"));
    INFO("scene paths " << s.size() << ", track paths " << c.size()); // 712 and 44 with the effect catalogue
    CHECK(s.size() < 4000);
}

#include "core/edit/SceneManagerModel.h"
#include "core/edit/StepCopyGesture.h"

TEST_CASE("edit.steps: hold COPY + steps, release, hold PASTE + a step: the panel gesture (EDT-005)", "[edit]")
{
    edittest::Bus b;
    ce::InlineWorkerPool pool;
    ce::SceneEditor editor(b.bus, pool);
    ce::StepCopyGesture g(editor);
    b.project().scenes[0].patterns[0].length = 32;
    const m::Pattern src = b.pattern(0, 0);
    g.setTarget(0, 0, 2, 1); // track 3, page 2 shown (steps 17…32)
    CHECK_FALSE(g.stepKey(1)); // nothing held: the step keys belong to the sequencer
    g.copyPressed();
    CHECK(g.active());
    CHECK(g.stepKey(1));
    CHECK(g.stepKey(3));
    CHECK(g.pending() == ((1ull << 16) | (1ull << 18)));
    g.copyReleased();
    CHECK(editor.isCopied(ce::stepsRef(0, 0, 2, (1ull << 16) | (1ull << 18))));
    g.pastePressed();
    CHECK(g.stepKey(16)); // target step 32: 32, 34 → wraps to 2 (span 3 ≤ 32)
    g.pasteReleased();
    CHECK(cell(b.pattern(0, 0), 31, 2) == cell(src, 16, 2));
    CHECK(cell(b.pattern(0, 0), 1, 2) == cell(src, 18, 2));
    CHECK(b.undoDepth() == 1);
    // COPY alone copies the shown bar.
    g.copyPressed();
    g.copyReleased();
    CHECK(editor.isCopied(ce::pageRef(0, 0, 2, 1)));
    CHECK(editor.clipboard().source()->kind == ce::ObjectKind::Page);
}

TEST_CASE("edit.steps: EDT-009 clear confirmations follow the preference, a scene always asks", "[edit]")
{
    CHECK(ce::clearNeedsConfirm(ce::ObjectKind::Scene, false));
    CHECK(ce::clearNeedsConfirm(ce::ObjectKind::Pattern, true));
    CHECK_FALSE(ce::clearNeedsConfirm(ce::ObjectKind::Pattern, false));
    CHECK(ce::clearNeedsConfirm(ce::ObjectKind::TrackWithSequences, true));
    CHECK(ce::clearNeedsConfirm(ce::ObjectKind::Sequence, true));
    CHECK_FALSE(ce::clearNeedsConfirm(ce::ObjectKind::Track, true));
    CHECK_FALSE(ce::clearNeedsConfirm(ce::ObjectKind::Steps, true));
}
