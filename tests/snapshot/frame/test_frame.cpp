// Frame components against the spec's mockups (gate ≥ 97 % within the stated tolerance): HOME as a whole,
// StatusRow / Breadcrumb / StatusIndicators, ContextBar (menu, track context with BACK, TRACK TYPE + BYPASS),
// KnobStrip + StripTab + ParamColumns (rest and step hold with a lock), RailItems.
#include "tests/snapshot/frame/Fixture.h"
#include "ui/shell/Layout.h"

#include <catch2/catch_test_macros.hpp>

using namespace snapshot;
namespace L = ui::layout;

TEST_CASE("frame: HOME matches home.svg", "[snapshot][frame]")
{
    Session session;
    ShellFixture f;
    const auto ours = f.image();
    const auto& mock = mockup("home");
    REQUIRE(mock.w == 1280);
    CHECK(compare(ours, mock, L::canvas, "frame-home").match >= kGate);
    CHECK(compare(ours, mock, L::statusRow, "frame-status-row").match >= kGate);
    CHECK(compare(ours, mock, L::contextBar, "frame-context-bar-menu").match >= kGate);
    CHECK(compare(ours, mock, L::knobStrip, "frame-knob-strip").match >= kGate);
    CHECK(compare(ours, mock, {0, L::stripY + L::tabH, 1280, L::stripH - L::tabH}, "frame-param-columns").match >=
          kGate);
    writePng(f.image(800, 480), "frame-home-800.png");
}

TEST_CASE("frame: playing with step 3 held and a PITCH lock matches home-playing-step-hold.svg", "[snapshot][frame]")
{
    Session session;
    ShellFixture f;
    auto& st = f.shell->state();
    st.playing = true;
    st.position.set("001:02:08");
    f.press(io::step(3));
    f.turn(io::knob(3), 170); // PITCH lock +17.0 (coarse 0.1 st per detent)
    const auto ours = f.image();
    const auto& mock = mockup("home-playing-step-hold");
    CHECK(f.shell->column(2).locked);
    CHECK(compare(ours, mock, L::knobStrip, "frame-knob-strip-step-hold").match >= kGate);
    CHECK(compare(ours, mock, L::statusRow, "frame-status-row-playing").match >= kGate);
}

namespace
{
constexpr ui::RailItemSpec kPlaybackRail[] = {{ui::RailItemSpec::Kind::Button, "SET SLICE"},
                                              {ui::RailItemSpec::Kind::Button, "SET SCALE"},
                                              {ui::RailItemSpec::Kind::Toggle, "REVERSE"},
                                              {ui::RailItemSpec::Kind::Toggle, "GRID SNAP"}};
} // namespace

TEST_CASE("frame: track context bar with BACK, breadcrumb and rail match playback.svg", "[snapshot][frame]")
{
    Session session;
    ShellFixture f({{.id = "track.menu",
                     .title = "TRACK MENU",
                     .parent = "home",
                     .body = ui::BodyTemplate::Full,
                     .contextBar = ui::ContextBarKind::TrackTypeBypass,
                     .rail = {},
                     .bodyComponent = {},
                     .strip = {}},
                    {.id = "track.playback",
                     .title = "PLAYBACK",
                     .parent = "track.menu",
                     .body = ui::BodyTemplate::RailContent,
                     .contextBar = ui::ContextBarKind::TrackContext,
                     .rail = kPlaybackRail,
                     .bodyComponent = {},
                     .strip = {"PLAYBACK", {}}}});
    auto& st = f.shell->state();
    st.selectedTrack = 5;
    st.playing = true;
    st.position.set("001:03:07");
    REQUIRE(f.shell->open("track.playback"));
    f.settle();
    f.shell->focus().focused = -1; // the mockup shows no rail focus
    const auto ours = f.image();
    const auto& mock = mockup("playback");
    CHECK(compare(ours, mock, L::contextBar, "frame-context-bar-track").match >= kGate);
    CHECK(compare(ours, mock, L::statusRow, "frame-status-row-breadcrumb").match >=
          kGate); // HOME › TRACK MENU › PLAYBACK
    // Rail: REVERSE off, GRID SNAP on — the mockup's toggle states (GRID SNAP is unbound here, so compare 3 items).
    CHECK(compare(ours, mock, {16, 152, 192, 338}, "frame-rail").match >= kGate);
}

TEST_CASE("frame: TRACK TYPE + BYPASS context bar matches track-menu-sample.svg", "[snapshot][frame]")
{
    Session session;
    ShellFixture f;
    f.shell->state().selectedTrack = 5;
    REQUIRE(f.shell->open("track.menu"));
    const auto ours = f.image();
    CHECK(compare(ours, mockup("track-menu-sample"), L::contextBar, "frame-context-bar-typebypass").match >= kGate);
}
