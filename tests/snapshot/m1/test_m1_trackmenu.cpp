// TRACK MENU per track type against track-menu-sample / -thru / -midi .svg and the TRACK TYPE popup; focus model
// (open focus, two-tap, BACK returns to the module), step-hold lock badges; HOME → TRACK MENU → PLAYBACK → BACK walk;
// the AMP ENVELOPE card's thumbnail against the strip's ATTACK / HOLD / RELEASE detents, INFINITY and SYNC.
#include "tests/snapshot/m1/DemoSample.h"
#include "tests/snapshot/m1/M1Fixture.h"
#include "ui/shell/Layout.h"
#include "ui/widgets/ModuleCard.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

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

namespace
{
// The AMP ENVELOPE card's thumbnail as drawn: its opaque stroke inside the thumbnail box (ModuleCard: 152 × 88 at
// card + (16, 44); the 12 % fill under it is translucent). `xa` / `xPeak` are the left and right ends of the stroke
// on the peak row (attack corner; end of HOLD, or the right end when RELEASE is INFINITY), `xr` its right end.
struct EnvThumb
{
    float xa = 1e9f, xPeak = -1e9f, xr = -1e9f, right = 0;
};
EnvThumb envThumb(const ui::DrawList& dl)
{
    const auto r = ui::ModuleCard::rect(1);
    const ui::RectF t{r.x + 16, r.y + 44, 152, 88};
    EnvThumb e;
    e.right = t.right() - 14;
    const float peakRow = t.y + 12 - 1; // the 2 lpx stroke's top at the peak
    for (const auto& c : dl.commands())
    {
        const auto& q = c.rect;
        if (c.op != ui::DrawCmd::Op::Fill || (c.argb >> 24) != 0xFF || q.w >= t.w || q.x < t.x || q.y < t.y ||
            q.x + q.w > t.right() || q.y + q.h > t.bottom())
            continue;
        e.xr = std::max(e.xr, q.x + q.w - 1);
        if (std::abs(q.y - peakRow) < 0.01f)
        {
            e.xa = std::min(e.xa, q.x + 1);
            e.xPeak = std::max(e.xPeak, q.x + q.w - 1);
        }
    }
    return e;
}

// TRACK MENU on track 5 with the AMP ENVELOPE card focused (strip: VELOCITY, ATTACK, HOLD, RELEASE, TRIGGER MODE).
struct AmpCardFixture : MenuFixture
{
    AmpCardFixture() : MenuFixture(5) { focusCard(1); }
    EnvThumb thumb() { return envThumb(frame()); }
};
} // namespace

TEST_CASE("ui.m1.trackMenu: each AMP ENVELOPE knob detent moves only its own thumbnail corner, forward", "[m1]")
{
    AmpCardFixture f;
    REQUIRE(f.shell->stripTab() == "AMP ENVELOPE");
    struct Sweep
    {
        int knob, detents;           // strip knob 2 ATTACK, 3 HOLD, 4 RELEASE, turned up from 0
        float attack, hold, release; // the values before
    };
    // ATTACK with the defaults (HOLD INFINITY, RELEASE 0); HOLD up to INFINITY with a RELEASE; RELEASE with a HOLD.
    for (const auto& s : {Sweep{2, 127, 0, 128, 0}, Sweep{3, 128, 0, 0, 64}, Sweep{4, 127, 0, 64, 0}})
    {
        CAPTURE(s.knob);
        f.set("env.attack", 5, s.attack);
        f.set("env.hold", 5, s.hold);
        f.set("env.release", 5, s.release);
        auto prev = f.thumb();
        for (int i = 1; i <= s.detents; ++i)
        {
            CAPTURE(i);
            f.turn(io::knob(s.knob), 1);
            const auto e = f.thumb();
            const float da = e.xa - prev.xa, dh = e.xPeak - prev.xPeak;
            const float dr = (e.xr - e.xPeak) - (prev.xr - prev.xPeak); // the release's length
            CHECK((s.knob == 2 ? da > 0.2f : std::abs(da) < 1e-3f));
            CHECK((s.knob == 3 ? dh > 0.2f : std::abs(dh) < 1e-3f));
            CHECK((s.knob == 4 ? dr > 0.2f : std::abs(dr) < 1e-3f));
            prev = e;
        }
    }
}

TEST_CASE("ui.m1.trackMenu: the AMP ENVELOPE thumbnail keeps the peak at RELEASE INFINITY and follows SYNC", "[m1]")
{
    AmpCardFixture f;
    f.set("env.hold", 5, 64);
    f.set("env.release", 5, 128); // INFINITY: the level stays where the release starts
    const auto e = f.thumb();
    CHECK(e.xPeak == Catch::Approx(e.right).margin(0.01));
    CHECK(e.xr == Catch::Approx(e.right).margin(0.01));
    // SYNC: the times are note values at the project tempo, so doubling the tempo halves the attack.
    f.set("env.release", 5, 0);
    f.set("env.attack", 5, 64);
    f.set("env.sync", 5, 1);
    f.shell->state().bpm.format("%.1f", 120.0);
    const float at120 = f.thumb().xa;
    f.shell->state().bpm.format("%.1f", 240.0);
    CHECK(f.thumb().xa < at120 - 1.0f);
}
