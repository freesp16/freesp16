// Slice points and the engine's slice table (SLC-001…004, SLC-011): the equal grid over the whole
// sample, moved and added points (stored as (tag, frame) pairs, only edits), AREA paging, pad → slice, GRID SNAP.
#include "core/commands/slice/SlicePoints.h"
#include "core/util/slice/SliceTable.h"
#include "engine/voice/SliceMap.h"

#include <catch2/catch_test_macros.hpp>

namespace cs = core::slice;
using core::model::SliceSettings;

TEST_CASE("slice.points: an untouched grid is N equal slices over the whole sample (SLC-001)", "[slice]")
{
    SliceSettings s; // gridCount 16, no edits
    CHECK(s.grids.empty());
    const auto p = cs::slicePoints(s, 176400);
    REQUIRE(p.count == 16);
    CHECK(p.start(0) == 0);
    CHECK(p.end(0) == 11025); // enumerations.md: slice 1 of a 176400-frame sample in 16 = 0 … 11025
    for (int i = 0; i < 16; ++i)
    {
        CHECK(p.start(i) == static_cast<std::int64_t>(i) * 176400 / 16);
        CHECK_FALSE(p.point[static_cast<std::size_t>(i)].moved);
    }
    CHECK(p.end(15) == 176400);
    for (const int n : cs::kGridCounts)
    {
        s.gridCount = n;
        CHECK(cs::slicePoints(s, 64000).count == n);
    }
    CHECK(cs::slicePoints(s, 0).count == 0); // no sample: no slices
}

TEST_CASE("slice.points: moved and added points, sorted; empty slices are dropped (SLC-002)", "[slice]")
{
    SliceSettings s;
    s.gridCount = 4; // 0, 1000, 2000, 3000 of 4000
    s.grids = {2, 2500, cs::kAddedTag, 500};
    const auto p = cs::slicePoints(s, 4000);
    REQUIRE(p.count == 5);
    CHECK(p.start(0) == 0);
    CHECK(p.start(1) == 500);
    CHECK(p.point[1].grid == -1);
    CHECK(p.start(2) == 1000);
    CHECK(p.start(3) == 2500);
    CHECK(p.point[3].grid == 2);
    CHECK(p.point[3].moved);
    CHECK(p.start(4) == 3000);
    CHECK(p.end(4) == 4000);
    CHECK(cs::editedPoints(s) == 2);

    s.grids = {cs::kAddedTag, 1000, cs::kAddedTag, 9999}; // on a grid point / past the end
    CHECK(cs::slicePoints(s, 4000).count == 4);
    s.gridCount = 0; // invalid → default
    CHECK(cs::slicePoints(s, 4000).count == 16);
}

TEST_CASE("slice.points: AREA pages 16 slices; pad 0 is the bottom-left = slice 16·area (SLC-003)", "[slice]")
{
    CHECK(cs::areaCount(16) == 1);
    CHECK(cs::areaCount(64) == 4);
    CHECK(cs::areaCount(17) == 2);
    CHECK(cs::sliceForPad(0, 0, 16) == 0);
    CHECK(cs::sliceForPad(15, 0, 16) == 15);
    CHECK(cs::sliceForPad(0, 3, 64) == 48);
    CHECK(cs::sliceForPad(15, 3, 64) == 63);
    CHECK(cs::sliceForPad(9, 0, 8) == -1); // 8 slices: pads 9–16 are empty
    CHECK(cs::sliceForPad(0, 9, 64) == 48); // area clamped
}

TEST_CASE("slice.table: the engine table holds ≤ 65 offsets; pad → region is O(1); GRID SNAP (SLC-004/011)", "[slice]")
{
    SliceSettings s;
    s.gridCount = 64;
    s.windowOffset = 2;
    const cs::SliceTable t = cs::buildTable(s, 64 * 100);
    CHECK(t.count() == 64);
    CHECK(t.windowOffset() == 2);
    CHECK(t.points[64] == 6400);
    CHECK(t.sliceForPad(0, t.windowOffset()) == 32);
    CHECK(t.region(32) == cs::SliceRegion {3200, 3300});
    CHECK(t.region(63) == cs::SliceRegion {6300, 6400});
    CHECK(t.region(64) == cs::SliceRegion {});
    CHECK(t.snap(3249) == 3200);
    CHECK(t.snap(3250) == 3200); // tie → the earlier point
    CHECK(t.snap(3251) == 3300);
    CHECK(t.snap(6390) == 6400); // the end is a grid line too
    CHECK(t.snap(-5) == 0);
    s.windowOffset = 7;
    CHECK(cs::buildTable(s, 6400).windowOffset() == 3);

    // The engine's view: the table built for this buffer, else the default 16 equal slices.
    engine::voice::SliceRegion r;
    REQUIRE(engine::voice::sliceRegion(&t, 6400, 5, r));
    CHECK(r == cs::SliceRegion {500, 600});
    REQUIRE(engine::voice::sliceRegion(&t, 1600, 5, r)); // stale table (another sample): 16 equal slices
    CHECK(r == cs::SliceRegion {500, 600});
    CHECK_FALSE(engine::voice::sliceRegion(&t, 1600, 16, r));
    CHECK_FALSE(engine::voice::sliceRegion(nullptr, 0, 0, r));
}
