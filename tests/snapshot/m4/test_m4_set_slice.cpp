// SET SLICE (ui/screens/setslice; 07-ui-design/screens/playback.md "SET SLICE", "GRID SLICE popup";
// UID-708…710, UI-298, UI-344): set-slice.svg (track 5, the 2-bar beat of 176400 frames, 16 slices, slice 1 current,
// ADD SLICE focused, DELETE SLICE disabled) and set-slice-grid-popup.svg; then the panel-only behaviour — POSITION
// moves the current slice's start (one undo step), ADD / DELETE, GRID SLICE with the reset message, AREA, a pad hit in
// SLICE mode selecting its slice — all through core/commands/slice on the document (the m2::Session).
#include "core/commands/slice/SliceCommands.h"
#include "core/util/playback/Grid.h"
#include "tests/snapshot/m1/DemoSample.h"
#include "tests/snapshot/m4/M4Fixture.h"
#include "ui/screens/setslice/SliceSync.h"
#include "ui/shell/Layout.h"
#include "ui/widgets/ListPopup.h"
#include "ui/widgets/WaveformEditor.h"

#include <catch2/catch_test_macros.hpp>

using namespace snapshot;
using m4test::M4Fixture;
namespace L = ui::layout;
namespace sc = core::commands::slice;
namespace cs = core::slice;

namespace
{
constexpr std::int64_t kFrames = 176400;

struct SliceFixture
{
    M4Fixture m;
    m1test::DemoSample beat {"beat"};
    SliceFixture()
    {
        ui::m1::SampleViews::instance().setProvider(
            [this](int t) { return t == 5 ? beat.view("Beat_120bpm_2bar") : ui::m1::SampleView {}; });
        ui::slice::sliceLive() = ui::slice::SliceLive {};
        auto& st = m.state();
        st.selectedTrack = 5;
        st.tracks[4].typeBadge = "SMPL";
        REQUIRE(m.open("track.menu"));
        REQUIRE(m.open("track.playback"));
        REQUIRE(m.open("track.playback.setSlice"));
    }
    ~SliceFixture()
    {
        ui::m1::SampleViews::instance().setProvider({});
        ui::slice::sliceLive() = ui::slice::SliceLive {};
    }
    const core::model::SliceSettings& slices() { return sc::settings(ui::m2::Session::instance().project(), 0, 4); }
    cs::SlicePoints points() { return cs::slicePoints(slices(), kFrames); }
    void knob(int n, int detents)
    {
        m.f.turn(io::knob(n), detents);
        m.f.settle();
    }
    void push()
    {
        m.f.press(io::Control::SelectorPush);
        m.f.settle();
    }
};
} // namespace

TEST_CASE("snapshot.m4.set_slice: SET SLICE (ui/screens/setslice) matches set-slice.svg", "[snapshot][m4]")
{
    Session session;
    SliceFixture s;
    CHECK(s.m.shell().current().id == "track.playback.setSlice");
    CHECK(s.m.shell().railFocus() == 0); // ADD SLICE holds the focus
    CHECK(s.m.hasText("SET SLICE"));
    CHECK(s.m.hasText("11025"));    // END of slice 1 of 16
    CHECK(s.m.hasText("0:00.250")); // its time sub-readout
    CHECK(s.m.hasText("16"));       // the last box of the AREA
    const auto ours = s.m.f.image();
    const auto& mock = snapshot::mockup("set-slice");
    CHECK(compare(ours, mock, ui::WaveformEditor::kEditor, "m4-set-slice-editor").match >= kGate);
    CHECK(compare(ours, mock, ui::WaveformEditor::kOverview, "m4-set-slice-overview").match >= kGate);
    CHECK(compare(ours, mock, {16, 152, 192, 338}, "m4-set-slice-rail").match >= kGate);
    CHECK(compare(ours, mock, {0, 624, 1280, 144}, "m4-set-slice-strip").match >= kGate);
    CHECK(compare(ours, mock, L::canvas, "m4-set-slice").match >= kGate);
}

TEST_CASE("snapshot.m4.set_slice_grid_popup: GRID SLICE opens the list of set-slice-grid-popup.svg", "[snapshot][m4]")
{
    Session session;
    SliceFixture s;
    s.m.f.turn(io::Control::Selector, 2); // ADD → DELETE (disabled, still reachable) → GRID SLICE
    s.m.f.settle();
    REQUIRE(s.m.shell().railFocus() == 2);
    s.push();
    REQUIRE(s.m.shell().popupDepth() == 1);
    const auto* list = dynamic_cast<const ui::ListPopup*>(s.m.shell().topPopup());
    REQUIRE(list != nullptr);
    CHECK(list->focusedItem() == 3); // 16 focused and ✓
    CHECK(s.m.hasText("GRID SLICE"));
    const auto ours = s.m.f.image();
    const auto& mock = snapshot::mockup("set-slice-grid-popup");
    CHECK(compare(ours, mock, {320, 228, 640, 312}, "m4-set-slice-grid-popup-list").match >= kGate);
    CHECK(compare(ours, mock, L::canvas, "m4-set-slice-grid-popup").match >= kGate);
    // 64 re-slices at once (no point was moved: no message), one undo step.
    s.m.f.turn(io::Control::Selector, 2);
    s.push();
    CHECK(s.m.shell().popupDepth() == 0);
    CHECK(s.slices().gridCount == 64);
    CHECK(s.points().count == 64);
    CHECK(ui::m2::Session::instance().undo());
    CHECK(s.slices().gridCount == 16);
}

TEST_CASE("m4ui.set_slice: POSITION moves the current slice's start; SLICE No. selects; detents coalesce", "[m4]")
{
    Session session;
    SliceFixture s;
    s.knob(1, 3); // SLICE No. 4
    CHECK(s.m.hasText("4"));
    CHECK(s.m.hasText("33075")); // START of slice 4 (3 · 11025)
    const auto coarse = core::playback::coarseStep(kFrames);
    s.knob(2, 2); // POSITION: 2 coarse detents (DemoSample: no buffer → no zero-crossing snap)
    CHECK(s.points().start(3) == 33075 + 2 * coarse);
    CHECK(s.points().point[3].moved);
    s.m.f.press(io::Control::Shift);
    s.knob(2, -1); // SHIFT: 1 frame
    s.m.f.release(io::Control::Shift);
    CHECK(s.points().start(3) == 33075 + 2 * coarse - 1);
    CHECK(s.m.hasText(std::to_string(33075 + 2 * coarse - 1)));
    // START / END are readouts: turning them changes nothing.
    s.knob(3, 5);
    s.knob(4, 5);
    CHECK(s.points().start(3) == 33075 + 2 * coarse - 1);
    // The moves coalesce into one undo step.
    CHECK(ui::m2::Session::instance().undo());
    CHECK(s.slices().grids.empty());
}

TEST_CASE("m4ui.set_slice: ADD SLICE / DELETE SLICE (disabled in place for grid points) and the GRID SLICE reset",
          "[m4]")
{
    Session session;
    SliceFixture s;
    s.knob(1, 2); // slice 3
    s.push();     // ADD SLICE (focused): the mid-point of slice 3
    REQUIRE(s.points().count == 17);
    CHECK(s.points().start(3) == 2 * 11025 + 11025 / 2);
    CHECK(s.m.hasText("4")); // the new slice is selected (slice 4)
    s.m.f.turn(io::Control::Selector, 1); // DELETE SLICE: enabled now
    s.m.f.settle();
    s.push();
    CHECK(s.points().count == 16);
    s.push(); // slice 3 is a grid point: disabled, nothing happens
    CHECK(s.points().count == 16);
    // A moved point: GRID SLICE asks before re-slicing ("Reset 1 moved slice point?"); CANCEL keeps it.
    s.knob(2, 1);
    REQUIRE(cs::editedPoints(s.slices()) == 1);
    s.m.f.turn(io::Control::Selector, 1);
    s.push();
    REQUIRE(s.m.shell().popupDepth() == 1);
    s.m.f.turn(io::Control::Selector, -1); // 8
    s.push();
    REQUIRE(s.m.shell().popupDepth() == 1); // the message
    CHECK(s.m.hasText("Reset 1 moved slice point?"));
    s.push(); // CANCEL (the default)
    CHECK(s.slices().gridCount == 16);
    CHECK(cs::editedPoints(s.slices()) == 1);
    s.push(); // the list again
    s.m.f.turn(io::Control::Selector, -1);
    s.push();
    s.m.f.turn(io::Control::Selector, 1); // CANCEL → RESET
    s.push();
    CHECK(s.slices().gridCount == 8);
    CHECK(s.slices().grids.empty());
}

TEST_CASE("m4ui.set_slice: AREA pages a 64-slice grid; a SLICE-mode pad hit selects its slice", "[m4]")
{
    Session session;
    SliceFixture s;
    auto& ses = ui::m2::Session::instance();
    ses.execute(sc::setCount(ses.project(), 0, 4, 64));
    s.m.f.settle();
    s.knob(5, 2); // AREA 3: slices 33–48
    CHECK(s.slices().windowOffset == 2);
    CHECK(s.m.hasText("33"));
    CHECK(s.m.hasText("48"));
    CHECK_FALSE(s.m.hasText("49"));
    s.knob(5, 5);
    CHECK(s.slices().windowOffset == 3); // clamped to the last area
    // The document saw a SLICE pad hit on pad 2 (slice 50): SLICE No. follows.
    ui::slice::sliceLive().onHit(4, 49, 1000.0);
    s.m.f.settle();
    CHECK(s.m.hasText("50"));
    CHECK(ui::slice::padSlice(ses.project(), 0, 4, 1, kFrames) == 49);
}
