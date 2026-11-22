// Snapshot tests of the effect screens: the FX1 / FX2 insert page, the MASTER/SEND FX bus page and SELECT FX,
// rendered through the Shell with the test-only effect world (FxFixture.h: the spec's per-effect table, no DSP) in the
// mockups' demo state, compared with the spec mockups under the harness tolerance (5 × 5 blur, channel difference
// ≤ 24, ≥ 97 % of the pixels).
#include "tests/snapshot/SnapshotTest.h"
#include "tests/snapshot/frame/Fixture.h"
#include "tests/snapshot/m3/FxFixture.h"
#include "ui/shell/Layout.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace snapshot;
namespace fx = ui::fx;

namespace
{
// The pinned spec mockups (resources/design/mockups/, hashes in resources/design/SPEC_VERSION).
const Image& m3Mockup(std::string_view name)
{
    return mockup(name);
}

struct Fx
{
    fxtest::World& world = fxtest::World::get();
    fx::ScopedFxEnvironment env{world.environment()};
    fxtest::FxShell f{measure()};

    Fx()
    {
        world.slots.reset();
        homeTracks(f.shell->state());
        f.shell->state().selectedTrack = 5;
    }
    void open(std::string_view screen)
    {
        REQUIRE(f.shell->open(screen));
        f.settle();
        REQUIRE(f.panel() != nullptr);
    }
    double check(std::string_view mock, std::string_view name)
    {
        const auto ours = render(f.settle());
        const auto r = compare(ours, m3Mockup(mock), ui::layout::canvas, name);
        return r.match;
    }
};

const float kCutoffLock = std::log(120.0f) / std::log(1000.0f); // 2.4 kHz in the fixture's log mapping
} // namespace

TEST_CASE("snapshot.m3.fx_filter: FX1 MULTI MODE FILTER, step 5 held with a CUT OFF lock (fx-filter.svg)",
          "[snapshot][m3]")
{
    Session session;
    Fx t;
    t.world.slots.selectEffect(fx::Slot::Fx1, 1, 5, "filter");
    auto& st = t.f.shell->state();
    st.playing = true;
    st.position.set("001:02:05");
    t.open("track.fx1");
    const auto& cutoff = t.world.spec("fx1.filter.cutoff");
    t.f.shell->values().setLock(cutoff, st.trackScope(5), 5, kCutoffLock);
    t.f.press(io::step(5));
    CHECK(t.check("fx-filter", "m3-fx-filter") >= kGate);
    t.f.release(io::step(5));
}

TEST_CASE("snapshot.m3.fx_delay_sync: FX1 DELAY with SYNC on, DIV column and the MIX RATIO hint (fx-delay-sync.svg)",
          "[snapshot][m3]")
{
    Session session;
    Fx t;
    t.world.slots.selectEffect(fx::Slot::Fx1, 1, 5, "delay");
    t.f.set("fx1.delay.sync", 1);
    t.open("track.fx1");
    t.f.panel()->setFocus(2); // SYNC (the first option cell)
    CHECK(t.f.panel()->syncOn());
    CHECK(t.f.panel()->mixHint());
    CHECK(t.check("fx-delay-sync", "m3-fx-delay-sync") >= kGate);
}

TEST_CASE("snapshot.m3.fx_none: FX1 with no effect, SELECT FX focused (fx-none.svg)", "[snapshot][m3]")
{
    Session session;
    Fx t;
    t.open("track.fx1");
    t.f.panel()->setFocus(0);
    CHECK(t.check("fx-none", "m3-fx-none") >= kGate);
}

TEST_CASE("snapshot.m3.fx_master_send_reverb: MASTER/SEND FX, SEND page, REVERB (fx-master-send-reverb.svg)",
          "[snapshot][m3]")
{
    Session session;
    Fx t;
    t.world.slots.selectEffect(fx::Slot::Send, 1, 0, "reverb");
    t.open("mixer.busFx");
    t.f.panel()->bindSlot("send", 0);
    CHECK(t.check("fx-master-send-reverb", "m3-fx-master-send-reverb") >= kGate);
}

TEST_CASE("snapshot.m3.fx_select_insert: SELECT FX on FX1, NONE current, FILTER focused (fx-select-insert.svg)",
          "[snapshot][m3]")
{
    Session session;
    Fx t;
    t.open("track.fx1");
    t.f.panel()->openSelectFx();
    t.f.turn(io::Control::Selector, 4);
    CHECK(t.check("fx-select-insert", "m3-fx-select-insert") >= kGate);
}

TEST_CASE("snapshot.m3.fx_select_master_send: SELECT FX on SEND, REVERB current, DELAY focused "
          "(fx-select-master-send.svg)",
          "[snapshot][m3]")
{
    Session session;
    Fx t;
    t.world.slots.selectEffect(fx::Slot::Send, 1, 0, "reverb");
    t.open("mixer.busFx");
    t.f.panel()->bindSlot("send", 0);
    t.f.panel()->openSelectFx();
    t.f.turn(io::Control::Selector, -1);
    CHECK(t.check("fx-select-master-send", "m3-fx-select-master-send") >= kGate);
}

TEST_CASE("snapshot.m3.trackmenu_fx: the TRACK MENU's FX card names its effect and binds its knobs",
          "[snapshot][m3]")
{
    Session session;
    Fx t;
    t.world.slots.selectEffect(fx::Slot::Fx1, 1, 5, "delay");
    REQUIRE(t.f.shell->open("track.menu"));
    t.f.settle();
    CHECK(t.f.drewText("FX1 : DELAY")); // hardware feedback: the card read "FX1 : NONE" whatever the slot held
    CHECK(t.f.drewText("FX2 : NONE"));
    const auto first = [&] { return t.f.shell->strip().model().columns[0].spec; };
    for (int i = 0; i < 16 && ! (first() != nullptr && first()->id.substr(0, 4) == "fx1."); ++i) // walk the cards
    {
        t.f.turn(io::Control::Selector, 1);
        t.f.frame();
    }
    REQUIRE(first() != nullptr); // the shortcut knobs: the slot's effect's columns
    CHECK(first()->id.substr(0, 10) == "fx1.delay.");
    // Another effect chosen on the FX page (SELECT FX), then BACK: the card and its strip follow.
    REQUIRE(t.f.shell->open("track.fx1"));
    t.f.settle();
    t.world.slots.selectEffect(fx::Slot::Fx1, 1, 5, "filter");
    t.f.settle();
    t.f.shell->back();
    t.f.settle();
    for (int i = 0; i < 16 && ! (first() != nullptr && first()->id.substr(0, 4) == "fx1."); ++i)
    {
        t.f.turn(io::Control::Selector, 1);
        t.f.frame();
    }
    int filterColumns = 0;
    for (const auto& c : t.f.shell->strip().model().columns)
        filterColumns += c.spec != nullptr && c.spec->id.substr(0, 11) == "fx1.filter." ? 1 : 0;
    CHECK(filterColumns > 0);
    CHECK(t.f.drewText("FX1 : MULTI MODE FILTER"));
}

TEST_CASE("snapshot.m3.mixer_fx_setting: MIXER's FX SETTING shows the bus effects and opens the bus page",
          "[snapshot][m3]")
{
    Session session;
    Fx t;
    t.world.slots.selectEffect(fx::Slot::Master, 1, 5, "filter");
    REQUIRE(t.f.shell->open("mixer"));
    t.f.settle();
    CHECK(t.f.drewText("MULTI MODE FILTER")); // the MASTER row names the bus effect (it was fixed text)
    CHECK(t.f.drewText("BOTH"));
    t.f.tap(100, 460); // the MASTER row of TYPE
    t.f.settle();
    CHECK(t.f.shell->router().current().id == "mixer.busFx"); // TYPE opened no page before (unregistered id)
    CHECK(fx::busPage() == fx::Slot::Master);
    // A parity mode set on the card is the project's FX mode (the slot access), not the card's own state.
    t.world.slots.setMode(1, fx::FxMode::Send);
    t.f.shell->back();
    t.f.settle();
    REQUIRE(t.f.shell->router().current().id == "mixer");
    CHECK(t.f.drewText("SEND"));
    t.f.tap(100, 418); // the one TYPE row (SEND runs alone)
    t.f.settle();
    CHECK(fx::busPage() == fx::Slot::Send);
}
