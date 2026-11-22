// HOME and TRACK SETTING against their mockups (gate ≥ 97 %, 5 × 5 blur, max channel difference 24): the whole
// canvas, the tile grid, the MASTER meter; the playing state with a trigger flash and a muted track; the loading bar,
// the missing-sample badge and the meter-feed flash (UID-503) as behaviour checks.
#include "tests/snapshot/m1/M1Fixture.h"
#include "ui/screens/home/TrackGrid.h"
#include "ui/shell/Layout.h"

#include <catch2/catch_test_macros.hpp>

using namespace snapshot;
using m1test::M1Fixture;
namespace L = ui::layout;

namespace
{
constexpr ui::RectF kGrid{16, 152, 1144, 456};
constexpr ui::RectF kMeter{1176, 152, 88, 456};

bool hasText(const ui::DrawList& dl, std::string_view text)
{
    for (const auto& c : dl.commands())
        if (c.op == ui::DrawCmd::Op::Text && dl.textOf(c) == text)
            return true;
    return false;
}
} // namespace

TEST_CASE("ui.m1.home: HOME matches home.svg", "[snapshot][m1]")
{
    Session session;
    M1Fixture f;
    REQUIRE(f.shell->router().current().id == "home");
    REQUIRE(f.shell->router().current().bodyComponent == "TrackTile"); // the real HOME screen, not the shell's fallback
    const auto ours = f.image();
    const auto& mock = mockup("home");
    CHECK(compare(ours, mock, L::canvas, "m1-home").match >= kGate);
    CHECK(compare(ours, mock, kGrid, "m1-home-tiles").match >= kGate);
    CHECK(compare(ours, mock, kMeter, "m1-home-meter").match >= kGate);
    CHECK(compare(ours, mock, L::knobStrip, "m1-home-strip").match >= kGate);
    writePng(f.image(800, 480), "m1-home-800.png");
}

TEST_CASE("ui.m1.home: playing, track 5 hit, track 6 muted matches home-playing-step-hold.svg", "[snapshot][m1]")
{
    Session session;
    M1Fixture f;
    auto& st = f.shell->state();
    st.playing = true;
    st.position.set("001:02:08");
    st.tracks[5].muted = true;
    f.settle();
    st.tracks[4].lastHitMs = f.now + 2000; // the frame the image is built at: flash at full strength
    const auto ours = f.image();
    const auto& mock = mockup("home-playing-step-hold");
    CHECK(compare(ours, mock, kGrid, "m1-home-playing-tiles").match >= kGate);
}

TEST_CASE("ui.m1.home: meter-feed triggers flash the tile, loading shows a bar, missing shows the caution badge",
          "[m1]")
{
    M1Fixture f;
    auto& feeds = f.shell->feeds();
    engine::MeterFeed m{};
    engine::PlayheadFeed p{};
    feeds.inject(m, p);
    f.frame();
    m.trackPeak[2] = {0.5f, 0.5f}; // track 3 jumps to −6 dB: a trigger (UID-503)
    feeds.inject(m, p);
    f.frame();
    CHECK(f.now - feeds.lastHitMs(3) < 1);
    CHECK(f.shell->body()->dirty(f.now));
    ui::UiContext ctx{ui::theme::palette(), measure(), f.now, f.shell->focus()};
    auto* grid = dynamic_cast<ui::TrackGrid*>(f.shell->body());
    REQUIRE(grid != nullptr);
    CHECK(grid->model(3, ctx).flash > 0.9f);
    CHECK(ui::TrackTile::levelSegments(grid->model(3, ctx).levelDb) == 5);
    CHECK(grid->model(4, ctx).flash == 0);

    ui::m1::loadProgress()[0] = 0.5f;
    f.shell->state().tracks[1].missing = true;
    const auto& dl = f.frame();
    CHECK_FALSE(hasText(dl, "Kick_01")); // the progress bar replaces the name (UI-410)
    CHECK(hasText(dl, "Snare_01"));      // still drawn, in text-3 (UID-504)
    CHECK_FALSE(hasText(dl, "UNSAVED TAKE"));

    f.shell->state().tracks[1].unsavedTake = true; // a take played as RECORDED TEMP. (sampling.md W3)
    const auto& dl2 = f.frame();
    CHECK(hasText(dl2, "UNSAVED TAKE"));
    CHECK(hasText(dl2, "Snare_01"));
}

TEST_CASE("ui.m1.home: rotary wraps the selection, push opens TRACK MENU, a second tap opens it too", "[m1]")
{
    M1Fixture f;
    auto& st = f.shell->state();
    f.turn(io::Control::Selector, -1);
    CHECK(st.selectedTrack == 16);
    f.turn(io::Control::Selector, 2);
    CHECK(st.selectedTrack == 2);
    const auto tile5 = L::trackTile(5);
    f.tap(tile5.x + 100, tile5.y + 50);
    CHECK(st.selectedTrack == 5);
    CHECK(f.shell->router().current().id == "home");
    f.tap(tile5.x + 100, tile5.y + 50);
    f.settle();
    CHECK(f.shell->router().current().id == "track.menu");
    f.press(io::Control::Back);
    f.settle();
    CHECK(f.shell->router().current().id == "home");
    f.press(io::Control::SelectorPush);
    f.settle();
    CHECK(f.shell->router().current().id == "track.menu");
}

TEST_CASE("ui.m1.home: a THRU track empties strip columns 3-5, a MIDI track all of them", "[m1]")
{
    M1Fixture f;
    auto& st = f.shell->state();
    st.selectedTrack = 11; // THRU
    f.settle(3);
    CHECK(f.shell->boundColumn(0) != nullptr);
    CHECK(f.shell->boundColumn(2) == nullptr);
    CHECK(f.shell->boundColumn(5) != nullptr);
    st.selectedTrack = 10; // MIDI
    f.settle(3);
    CHECK(f.shell->boundColumn(0) == nullptr);
    st.selectedTrack = 1;
    f.settle(3);
    CHECK(f.shell->boundColumn(2) != nullptr);
}

TEST_CASE("ui.m1.trackSetting: TRACK SETTING matches track-setting.svg", "[snapshot][m1]")
{
    Session session;
    M1Fixture f;
    static constexpr std::string_view names[] = {"Kick_01",     "Snare_01",         "Hat_Closed_01",
                                                 "Hat_Open_01", "Beat_120bpm_2bar", "Bass_120bpm_2bar"};
    auto& st = f.shell->state();
    for (int t = 1; t <= 6; ++t)
    {
        auto& s = st.tracks[static_cast<std::size_t>(t - 1)];
        s.hasContent = true;
        s.name = names[t - 1];
        f.set("track.padVelocity", t, t <= 4 ? 1 : 0);
    }
    for (int t = 7; t <= 16; ++t)
        f.set("track.padVelocity", t, 0);
    f.set("track.chokeGroup", 3, 1);
    f.set("track.chokeGroup", 4, 1);
    for (int t = 1; t <= 16; ++t)
        f.set("track.padColor", t, static_cast<float>(t - 1));
    f.set("track.padColor", 5, 3); // track 5 being turned to colour #04
    st.selectedTrack = 5;
    REQUIRE(f.shell->open("trackSetting"));
    f.settle();
    f.turn(io::knob(3), 0);
    const auto ours = f.image();
    const auto& mock = m1test::m1mockup("track-setting");
    CHECK(compare(ours, mock, kGrid, "m1-track-setting-tiles").match >= kGate);
    CHECK(compare(ours, mock, L::contextBar, "m1-track-setting-context").match >= kGate);
    CHECK(compare(ours, mock, L::canvas, "m1-track-setting").match >= kGate);
    // TRACK SETTING never navigates (UID-530)
    f.press(io::Control::SelectorPush);
    f.settle();
    CHECK(f.shell->router().current().id == "trackSetting");
}
