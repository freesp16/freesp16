// Slice editor commands (SLC-001/002/006/010): GRID SLICE re-slices and discards edits, POSITION moves a
// point between its neighbours (coarse / SHIFT fine, detents coalesce), ADD / DELETE rules, AREA, one undo step per
// edit through the CommandBus, the ChangeSet path, and "slices are a view": no edit touches the sample.
#include "core/commands/CommandBusAccess.h"
#include "core/commands/slice/SliceCommands.h"
#include "core/model/ModelOps.h"
#include "core/util/playback/Grid.h"
#include "tests/unit/voice/VoiceHarness.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace sc = core::commands::slice;
namespace cs = core::slice;

namespace
{
constexpr std::int64_t kFrames = 160000; // 16 × 10000

struct Doc
{
    core::CommandBus bus;
    std::vector<std::string> paths;
    core::Subscription sub;
    Doc()
    {
        core::commands::replaceProject(bus, core::model::newDefaultProject());
        sub = bus.observe("", [this](const core::ChangeSet& c) { paths.insert(paths.end(), c.paths().begin(), c.paths().end()); });
    }
    const core::model::Project& p() const { return core::commands::project(bus); }
    const core::model::SliceSettings& s(int scene = 1, int track = 4) const { return sc::settings(p(), scene, track); }
    bool run(std::unique_ptr<core::Command> c)
    {
        if (c == nullptr)
            return false;
        bus.execute(std::move(c));
        core::commands::flushNotifications(bus);
        return true;
    }
};
} // namespace

TEST_CASE("slice.commands: paths round-trip; settings of scene / track", "[slice]")
{
    CHECK(sc::slicesPath(3, 11) == "scene[3].track[11].slices");
    int s = -1, t = -1;
    CHECK(sc::parseSlicesPath("scene[3].track[11].slices", s, t));
    CHECK((s == 3 && t == 11));
    CHECK_FALSE(sc::parseSlicesPath("scene[3].track[11].playback.start", s, t));
    CHECK_FALSE(sc::parseSlicesPath("scene[16].track[1].slices", s, t));
}

TEST_CASE("slice.commands: GRID SLICE sets N equal slices and discards edits; one undo step (SLC-001)", "[slice]")
{
    Doc d;
    REQUIRE(d.run(sc::movePoint(d.p(), 1, 4, 3, 31000, kFrames)));
    REQUIRE(d.run(sc::addPoint(d.p(), 1, 4, 5000, kFrames)));
    CHECK(cs::editedPoints(d.s()) == 2);
    CHECK(d.paths.back() == "scene[1].track[4].slices");
    REQUIRE(d.run(sc::setCount(d.p(), 1, 4, 64)));
    CHECK(d.s().gridCount == 64);
    CHECK(d.s().grids.empty());
    CHECK(cs::slicePoints(d.s(), kFrames).count == 64);
    CHECK(sc::setCount(d.p(), 1, 4, 64) == nullptr); // nothing to change
    REQUIRE(d.bus.undo());
    CHECK(d.s().gridCount == 16);
    CHECK(cs::editedPoints(d.s()) == 2); // the moved / added points are back
    CHECK(sc::setCount(d.p(), 1, 4, 5) != nullptr); // → 4 (the nearest valid count)
}

TEST_CASE("slice.commands: POSITION moves a point strictly between its neighbours; back on the grid = unmoved", "[slice]")
{
    Doc d;
    REQUIRE(d.run(sc::movePoint(d.p(), 1, 4, 2, 25000, kFrames)));
    auto pts = cs::slicePoints(d.s(), kFrames);
    CHECK(pts.start(2) == 25000);
    CHECK(pts.point[2].moved);
    REQUIRE(d.run(sc::movePoint(d.p(), 1, 4, 2, 0, kFrames))); // clamped to slice 2's start + 1
    CHECK(cs::slicePoints(d.s(), kFrames).start(2) == 10001);
    REQUIRE(d.run(sc::movePoint(d.p(), 1, 4, 2, 99999, kFrames)));
    CHECK(cs::slicePoints(d.s(), kFrames).start(2) == 29999);
    REQUIRE(d.run(sc::movePoint(d.p(), 1, 4, 2, 20000, kFrames))); // its grid position again
    CHECK(d.s().grids.empty());
    CHECK(sc::movePoint(d.p(), 1, 4, 16, 1, kFrames) == nullptr); // no such slice
    CHECK(sc::movePoint(d.p(), 1, 4, 2, 1, 0) == nullptr);        // no sample
}

TEST_CASE("slice.commands: POSITION detents (coarse frames/128, SHIFT 1 frame) coalesce into one undo step", "[slice]")
{
    Doc d;
    struct TestClock final : core::Clock
    {
        double now = 0;
        double nowMs() const noexcept override { return now; }
    } clock;
    core::commands::setClock(d.bus, &clock);
    double& now = clock.now;
    const auto coarse = core::playback::coarseStep(kFrames);
    REQUIRE(d.run(sc::nudgePoint(d.p(), 1, 4, 5, 1, false, kFrames)));
    now += 100;
    REQUIRE(d.run(sc::nudgePoint(d.p(), 1, 4, 5, 2, false, kFrames)));
    now += 100;
    REQUIRE(d.run(sc::nudgePoint(d.p(), 1, 4, 5, -1, true, kFrames)));
    CHECK(cs::slicePoints(d.s(), kFrames).start(5) == 50000 + 3 * coarse - 1);
    REQUIRE(d.bus.undo()); // one step back to the grid
    CHECK(d.s().grids.empty());
    CHECK_FALSE(d.bus.undo()); // nothing older
}

TEST_CASE("slice.commands: ADD SLICE and DELETE SLICE (grid points and slice 1 cannot be deleted)", "[slice]")
{
    Doc d;
    const auto& s0 = d.s();
    CHECK(sc::canAdd(s0, kFrames));
    CHECK_FALSE(sc::canDelete(s0, 0, kFrames));
    CHECK_FALSE(sc::canDelete(s0, 3, kFrames)); // only the grid: DELETE disabled
    CHECK(sc::addFrame(s0, 3, -1, kFrames) == 35000);   // the slice's mid-point
    CHECK(sc::addFrame(s0, 3, 31234, kFrames) == 31234); // the playhead inside it
    REQUIRE(d.run(sc::addPoint(d.p(), 1, 4, 35000, kFrames)));
    auto pts = cs::slicePoints(d.s(), kFrames);
    REQUIRE(pts.count == 17);
    CHECK(pts.start(4) == 35000);
    CHECK(sc::canDelete(d.s(), 4, kFrames));
    CHECK(sc::addPoint(d.p(), 1, 4, 35000, kFrames) == nullptr); // already a point
    CHECK(sc::addPoint(d.p(), 1, 4, 0, kFrames) == nullptr);
    // An added point moves like any other and stays deletable.
    REQUIRE(d.run(sc::movePoint(d.p(), 1, 4, 4, 36000, kFrames)));
    CHECK(cs::slicePoints(d.s(), kFrames).start(4) == 36000);
    REQUIRE(d.run(sc::deletePoint(d.p(), 1, 4, 4, kFrames)));
    CHECK(d.s().grids.empty());
    CHECK(sc::deletePoint(d.p(), 1, 4, 4, kFrames) == nullptr);
    // At most 64 points.
    REQUIRE(d.run(sc::setCount(d.p(), 1, 4, 64)));
    CHECK_FALSE(sc::canAdd(d.s(), kFrames));
    CHECK(sc::addPoint(d.p(), 1, 4, 1234, kFrames) == nullptr);
}

TEST_CASE("slice.commands: AREA pages within the grid and coalesces (SLC-003)", "[slice]")
{
    Doc d;
    CHECK(sc::setArea(d.p(), 1, 4, 1, kFrames) == nullptr); // 16 slices: one area
    REQUIRE(d.run(sc::setCount(d.p(), 1, 4, 64)));
    REQUIRE(d.run(sc::setArea(d.p(), 1, 4, 3, kFrames)));
    CHECK(d.s().windowOffset == 3);
    CHECK(sc::setArea(d.p(), 1, 4, 9, kFrames) == nullptr); // clamped to 3: unchanged
    REQUIRE(d.run(sc::setCount(d.p(), 1, 4, 16)));
    CHECK(d.s().windowOffset == 0); // re-slicing clamps the window
}

TEST_CASE("slice.commands: slices are a view — no edit changes the sample buffer (SLC-010)", "[slice]")
{
    std::vector<float> audio(static_cast<std::size_t>(kFrames));
    for (std::size_t i = 0; i < audio.size(); ++i)
        audio[i] = static_cast<float>((i * 2654435761u) % 1000u) / 1000.0f - 0.5f;
    voicetest::TestBuffer buf(audio);
    const auto hash = [&buf] {
        std::uint64_t h = 1469598103934665603ull;
        for (std::int64_t i = 0; i < buf.frames(); ++i)
        {
            const float v = buf.at(0, i);
            std::uint32_t bits;
            std::memcpy(&bits, &v, sizeof bits);
            h = (h ^ bits) * 1099511628211ull;
        }
        return h;
    };
    const auto before = hash();
    Doc d;
    d.run(sc::setCount(d.p(), 1, 4, 32));
    d.run(sc::movePoint(d.p(), 1, 4, 7, 37000, buf.frames()));
    d.run(sc::addPoint(d.p(), 1, 4, 77777, buf.frames()));
    const auto table = cs::buildTable(d.s(), buf.frames());
    CHECK(table.count() == 33);
    CHECK(hash() == before);
    CHECK(buf.get()->frames == kFrames);
}
