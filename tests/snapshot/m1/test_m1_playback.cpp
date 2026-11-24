// PLAYBACK (playback.svg: track 5, GRID SNAP on, loop from grid 5, playing at 62 %, L active), SET SCALE
// (set-scale.svg, MINOR) and SET SCALE / SET SLICE behaviour; zoom about the active cursor, overview tap.
#include "tests/snapshot/m1/DemoSample.h"
#include "tests/snapshot/m1/M1Fixture.h"
#include "ui/shell/Layout.h"

#include <catch2/catch_test_macros.hpp>

using namespace snapshot;
using m1test::M1Fixture;
namespace L = ui::layout;

namespace
{
struct PlaybackFixture : M1Fixture
{
    m1test::DemoSample beat{"beat"};
    PlaybackFixture()
    {
        ui::m1::SampleViews::instance().setProvider(
            [this](int t) { return t == 5 ? beat.view("Beat_120bpm_2bar") : ui::m1::SampleView{}; });
        auto& st = shell->state();
        st.selectedTrack = 5;
        st.tracks[4].typeBadge = "SMPL";
        REQUIRE(shell->open("track.menu"));
        settle();
        REQUIRE(shell->open("track.playback"));
        settle();
    }
    ~PlaybackFixture() { ui::m1::SampleViews::instance().setProvider({}); }
};
} // namespace

TEST_CASE("ui.m1.playback: PLAYBACK matches playback.svg", "[snapshot][m1]")
{
    Session session;
    PlaybackFixture f;
    auto& st = f.shell->state();
    st.playing = true;
    st.position.set("001:03:07");
    f.set("playback.gridSnap", 5, 1);
    f.set("playback.loop", 5, 1);
    f.set("playback.start", 5, 1);
    f.frame();
    f.set("playback.loopStart", 5, 5); // the last change: L is the active cursor
    f.set("playback.length", 5, 176400);
    engine::PlayheadFeed ph{};
    ph.trackPosition.fill(-1);
    ph.trackPosition[4] = 0.62f;
    f.shell->feeds().inject({}, ph);
    f.shell->focus().focused = -1;
    const auto ours = f.image();
    const auto& mock = m1test::m1mockup("playback");
    CHECK(compare(ours, mock, L::canvas, "m1-playback").match >= kGate);
    CHECK(compare(ours, mock, {224, 152, 960, 360}, "m1-playback-editor").match >= kGate);
    CHECK(compare(ours, mock, {224, 528, 960, 64}, "m1-playback-overview").match >= kGate);
    CHECK(compare(ours, mock, {1184, 260, 80, 144}, "m1-playback-zoom").match >= kGate);
    CHECK(compare(ours, mock, L::knobStrip, "m1-playback-strip").match >= kGate);
}

TEST_CASE("ui.m1.playback: the rotary zooms about the active cursor, an overview tap moves the window", "[m1]")
{
    PlaybackFixture f;
    f.set("playback.start", 5, 44100);
    f.frame();
    f.turn(io::Control::Selector, 4); // ×4
    f.frame();
    auto* body = f.shell->body();
    REQUIRE(body != nullptr);
    // The overview frame narrows to a quarter around START (frame 44100 = 25 %).
    const auto& dl = f.frame();
    bool narrowFrame = false;
    for (const auto& c : dl.commands())
        if (c.op == ui::DrawCmd::Op::Stroke && c.rect.y == 530 && c.rect.w > 200 && c.rect.w < 260)
            narrowFrame = true;
    CHECK(narrowFrame);
    f.tap(224 + 24 + 912 * 0.9f, 560);
    const auto& dl2 = f.frame();
    bool moved = false;
    for (const auto& c : dl2.commands())
        if (c.op == ui::DrawCmd::Op::Stroke && c.rect.y == 530 && c.rect.x > 224 + 24 + 912 * 0.7f)
            moved = true;
    CHECK(moved);
    // BACK returns to TRACK MENU
    f.press(io::Control::Back);
    f.settle();
    CHECK(f.shell->router().current().id == "track.menu");
}

TEST_CASE("ui.m1.playback: SET SLICE opens; SET SCALE matches set-scale.svg", "[snapshot][m1]")
{
    Session session;
    PlaybackFixture f;
    CHECK(f.shell->open("track.playback.setSlice")); // ui.setSlice is gone: SET SLICE is the reference screen
    f.press(io::Control::Back);
    f.settle();
    f.set("scale.type", 5, 6); // AEOLIAN (the mockup's MINOR)
    REQUIRE(f.shell->open("track.playback.setScale"));
    f.settle();
    f.shell->focus().focused = 0;
    const auto ours = f.image();
    const auto& mock = m1test::m1mockup("set-scale");
    CHECK(compare(ours, mock, {224, 152, 1040, 456}, "m1-set-scale-body").match >= kGate);
    CHECK(compare(ours, mock, {16, 152, 192, 102}, "m1-set-scale-rail").match >= kGate);
    CHECK(compare(ours, mock, L::canvas, "m1-set-scale").match >= kGate);
}
