// TRACK MENU per track type against track-menu-sample / -thru / -midi .svg and the TRACK TYPE popup; focus model
// (open focus, two-tap, BACK returns to the module), step-hold lock badges; HOME → TRACK MENU → PLAYBACK → BACK walk.
#include "tests/snapshot/m1/DemoSample.h"
#include "tests/snapshot/m1/M1Fixture.h"
#include "ui/shell/Layout.h"
#include "ui/widgets/ModuleCard.h"

#include <catch2/catch_test_macros.hpp>

using namespace snapshot;
using m1test::M1Fixture;
namespace L = ui::layout;

namespace
{
constexpr ui::RectF kPanel{16, 152, 1248, 456};
constexpr ui::RectF kCards{40, 264, 1200, 168};

struct MenuFixture : M1Fixture
{
    m1test::DemoSample beat{"beat"};
    explicit MenuFixture(int track)
    {
        ui::m1::SampleViews::instance().setProvider(
            [this](int t) { return t == 5 ? beat.view("Beat_120bpm_2bar") : ui::m1::SampleView{}; });
        shell->state().selectedTrack = track;
        REQUIRE(shell->open("track.menu"));
        settle();
    }
    ~MenuFixture() { ui::m1::SampleViews::instance().setProvider({}); }
    // Rotary to the given card slot (from the open focus).
    void focusCard(int slot)
    {
        const auto r = ui::ModuleCard::rect(slot);
        tap(r.x + 92, r.y + 76);
        settle();
    }
};
} // namespace

TEST_CASE("ui.m1.trackMenu: Sample track matches track-menu-sample.svg", "[snapshot][m1]")
{
    Session session;
    MenuFixture f(5);
    f.set("playback.loop", 5, 1);
    f.focusCard(0);
    CHECK(f.shell->stripTab() == "PLAYBACK");
    const auto ours = f.image();
    const auto& mock = m1test::m1mockup("track-menu-sample");
    CHECK(compare(ours, mock, L::canvas, "m1-track-menu-sample").match >= kGate);
    CHECK(compare(ours, mock, kPanel, "m1-track-menu-sample-panel").match >= kGate);
    CHECK(compare(ours, mock, kCards, "m1-track-menu-sample-cards").match >= kGate);
    CHECK(compare(ours, mock, {16, 152, 1248, 104}, "m1-track-menu-sample-header").match >= kGate);
}

TEST_CASE("ui.m1.trackMenu: Thru track matches track-menu-thru.svg", "[snapshot][m1]")
{
    Session session;
    MenuFixture f(11);
    f.focusCard(5);
    const auto ours = f.image();
    const auto& mock = m1test::m1mockup("track-menu-thru");
    CHECK(compare(ours, mock, kPanel, "m1-track-menu-thru-panel").match >= kGate);
}

TEST_CASE("ui.m1.trackMenu: MIDI track matches track-menu-midi.svg", "[snapshot][m1]")
{
    Session session;
    MenuFixture f(10);
    f.focusCard(0);
    const auto ours = f.image();
    const auto& mock = m1test::m1mockup("track-menu-midi");
    CHECK(compare(ours, mock, kPanel, "m1-track-menu-midi-panel").match >= kGate);
}

TEST_CASE("ui.m1.trackMenu: TRACK TYPE opens the type list (track-menu-type-popup.svg)", "[snapshot][m1]")
{
    Session session;
    MenuFixture f(5);
    f.tap(932, 88); // TRACK TYPE in the context bar
    f.settle();
    REQUIRE(f.shell->popupDepth() == 1);
    f.turn(io::Control::Selector, 2); // focus MIDI
    const auto ours = f.image();
    const auto& mock = m1test::m1mockup("track-menu-type-popup");
    CHECK(compare(ours, mock, {320, 252, 640, 224}, "m1-track-menu-type-popup").match >= kGate);
}

TEST_CASE("ui.m1.trackMenu: open focus BROWSE, two-tap cards, BACK returns to the module, lock badges", "[m1]")
{
    MenuFixture f(5);
    CHECK(f.shell->stripTab() == "BROWSE"); // UI-222
    const auto amp = ui::ModuleCard::rect(1);
    f.tap(amp.x + 92, amp.y + 76); // first tap: focus + strip
    f.settle();
    CHECK(f.shell->router().current().id == "track.menu");
    CHECK(f.shell->stripTab() == "AMP ENVELOPE");
    f.tap(amp.x + 92, amp.y + 76); // second tap: open
    f.settle();
    CHECK(f.shell->router().current().id == "track.ampEnvelope");
    f.press(io::Control::Back);
    f.settle();
    CHECK(f.shell->router().current().id == "track.menu");
    CHECK(f.shell->stripTab() == "AMP ENVELOPE"); // focus kept on the module (UI-009)
    // a held step with a lock on ATTACK badges the AMP ENVELOPE card (UID-658)
    f.press(io::step(3));
    f.turn(io::knob(2), 5);
    bool badge = false;
    for (const auto& c : f.frame().commands())
        if (c.op == ui::DrawCmd::Op::Fill && c.rect.x == amp.x + 184 - 14 && c.rect.y == amp.y + 6 && c.rect.w == 8)
            badge = true;
    CHECK(badge);
    f.release(io::step(3));
}

TEST_CASE("ui.m1.walk: HOME -> TRACK MENU -> PLAYBACK -> BACK with the focus restored", "[m1]")
{
    MenuFixture f(5);
    f.press(io::Control::Back);
    f.settle();
    REQUIRE(f.shell->router().current().id == "home");
    f.press(io::Control::SelectorPush); // HOME push → TRACK MENU
    f.settle();
    REQUIRE(f.shell->router().current().id == "track.menu");
    f.turn(io::Control::Selector, 2); // BROWSE → SAMPLING/EDIT → PLAYBACK
    f.settle();
    CHECK(f.shell->stripTab() == "PLAYBACK");
    f.press(io::Control::SelectorPush);
    f.settle();
    REQUIRE(f.shell->router().current().id == "track.playback");
    f.press(io::Control::Back);
    f.settle();
    CHECK(f.shell->router().current().id == "track.menu");
    CHECK(f.shell->stripTab() == "PLAYBACK");
}
