// SLICE pad mode on the message thread (SLC-003/005, UI-152, PAD-001): the pad lamps show the pads that
// hold a slice of the selected track in its colour (the rest dark) and flash on a hit; SliceLive keeps the last-played
// slice per track and pairs SLICE pad hits with the engine's RecordedHit (oldest first, within 1 s); padSlice follows
// the model's grid, AREA and the sample length the way the engine does.
#include "core/commands/CommandBusAccess.h"
#include "core/commands/slice/SliceCommands.h"
#include "core/model/ModelOps.h"
#include "tests/fakes/FakeEngineBridge.h"
#include "ui/input/pads/PadController.h"
#include "ui/screens/setslice/SliceSync.h"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <variant>

namespace
{
namespace cmd = core::commands;
using ui::input::pads::PadController;
using ui::input::pads::PadMode;

struct Lamps final : io::LampSink
{
    std::map<io::Control, std::pair<std::uint32_t, std::uint8_t>> lamps;
    void setLamp(io::Control c, std::uint32_t rgb, std::uint8_t level) override { lamps[c] = {rgb, level}; }
    void flush() override {}
};
} // namespace

TEST_CASE("slice.pads: SLICE lamps light the pads holding a slice and flash the hit one (UI-152)", "[slice]")
{
    core::CommandBus bus;
    cmd::replaceProject(bus, core::model::newDefaultProject());
    fakes::FakeEngineBridge bridge;
    ui::input::pads::SimpleSelection selection;
    selection.select(4);
    PadController pads(bus, bridge, selection);
    std::int64_t frames = 80000;
    pads.slicePad = [&](int track, int pad) { return ui::slice::padSlice(cmd::project(bus), 0, track, pad, frames); };
    pads.onModeKey(PadMode::Slice);
    bus.execute(cmd::slice::setCount(cmd::project(bus), 0, 4, 8));
    Lamps lamps;
    engine::MeterFeed meters;
    pads.paintLamps(lamps, 0.0, meters);
    CHECK(lamps.lamps[io::pad(1)].second > 0);
    CHECK(lamps.lamps[io::pad(8)].second > 0);
    CHECK(lamps.lamps[io::pad(9)].second == 0); // 8 slices: pads 9–16 dark
    CHECK(lamps.lamps[io::pad(16)].second == 0);
    CHECK(lamps.lamps[io::Control::PadModeSlice].second == 255);
    pads.onPad(2, 100, false, true, 100.0, false); // indirect source: the controller posts the hit with its pad
    pads.paintLamps(lamps, 100.0, meters);
    CHECK(lamps.lamps[io::pad(3)].first == 0xFFFFFF); // the hit flashes white …
    CHECK(lamps.lamps[io::pad(4)].first != 0xFFFFFF);
    pads.paintLamps(lamps, 1300.0, meters);
    CHECK(lamps.lamps[io::pad(3)].first != 0xFFFFFF); // … back to the track colour
    bool posted = false;
    for (const auto& m : bridge.posted)
        if (const auto* t = std::get_if<engine::TriggerNow>(&m); t != nullptr && t->pad == 2)
            posted = true;
    CHECK(posted); // the engine routes it to the selected track's slice
    frames = 0; // no sample: the engine's default view (16 slices)
    pads.paintLamps(lamps, 2000.0, meters);
    CHECK(lamps.lamps[io::pad(16)].second > 0);
}

TEST_CASE("slice.pads: padSlice follows grid, AREA and the sample length like the engine", "[slice]")
{
    auto p = core::model::newDefaultProject();
    CHECK(ui::slice::padSlice(*p, 0, 0, 5, 1000) == 5);
    p->scenes[0].tracks[0].slices.gridCount = 64;
    p->scenes[0].tracks[0].slices.windowOffset = 2;
    CHECK(ui::slice::padSlice(*p, 0, 0, 5, 64000) == 37);
    CHECK(ui::slice::padSlice(*p, 0, 0, 5, 0) == 5); // no sample length: the engine's default 16-slice grid
    CHECK(ui::slice::sliceCount(*p, 0, 0, 64000) == 64);
    p->scenes[0].tracks[0].slices.grids = {core::slice::kAddedTag, 1};
    CHECK(ui::slice::sliceCount(*p, 0, 0, 0) == 64); // capped
}

TEST_CASE("slice.pads: SliceLive pairs pad hits with recorded hits per track, oldest first, within 1 s", "[slice]")
{
    ui::slice::SliceLive live;
    CHECK(live.lastSlice[3] == -1);
    live.onHit(3, 7, 1000.0);
    live.onHit(5, 2, 1010.0);
    live.onHit(3, 9, 1020.0);
    CHECK(live.lastSlice[3] == 9);
    CHECK(live.takeRecorded(3, 1100.0) == 7);
    CHECK(live.takeRecorded(3, 1100.0) == 9);
    CHECK_FALSE(live.takeRecorded(3, 1100.0).has_value());
    CHECK_FALSE(live.takeRecorded(5, 2500.0).has_value()); // stale: never recorded
    for (int i = 0; i < 40; ++i)
        live.onHit(1, i % 16, 3000.0 + i);
    CHECK(live.numPending == ui::slice::SliceLive::kPending);
    CHECK(live.takeRecorded(1, 3050.0) == (40 - ui::slice::SliceLive::kPending) % 16);
}
