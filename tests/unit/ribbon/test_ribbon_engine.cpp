// engine::ribbon::RibbonEngine (touch-strip.md RIB-001…007/010): the laws, PITCH / USER through the
// matrix's set / clear, HOLD, the pad rule, REPEAT on the transport grid and free-running, the LED mask.
#include "tests/unit/ribbon/RibbonTestUtil.h"

#include "engine/modulation/ModMatrix.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace ribbontest;
using engine::ribbon::Mode;

namespace
{
// Compile check against the real ModMatrix (header only here: never called, so never linked).
[[maybe_unused]] inline void publishesToTheRealMatrix(engine::ribbon::RibbonEngine& r, engine::modulation::ModMatrix& m)
{
    r.publish(m);
}

float cents(const FakeMatrix& m, int track)
{
    const auto* e = m.at(track, id("playback.pitch"));
    return e == nullptr ? 0.0f : engine::modulation::pitchOffsetCents(e->value);
}

engine::ParamChange param(std::string_view name, float v, std::uint16_t scope = 0) { return {id(name), scope, v, 0}; }

void assign(engine::ribbon::RibbonEngine& r, int set, int item, std::string_view target, float bottom, float top)
{
    const auto scope = static_cast<std::uint16_t>(set * 8 + item);
    REQUIRE(r.onParam(param("ribbon.userTarget", static_cast<float>(id(target).value), scope)));
    REQUIRE(r.onParam(param("ribbon.userBottom", bottom, scope)));
    REQUIRE(r.onParam(param("ribbon.userTop", top, scope)));
}
} // namespace

TEST_CASE("ribbon.laws: PITCH, REPEAT zones, USER interpolation and the LED mask", "[ribbon]")
{
    namespace rb = core::ribbon;
    CHECK(rb::pitchCents(127, -200, 200) == 200.0f); // exactly +200 cents at the top (RIB-002)
    CHECK(rb::pitchCents(0, -200, 200) == -200.0f);
    CHECK(rb::pitchCents(64, -200, 200) == 0.0f);
    CHECK(rb::pitchCents(96, -200, 200) == Catch::Approx(200.0 * 32 / 63));
    CHECK(rb::bendCents(16383, -200, 200) == 200.0f);
    CHECK(rb::bendCents(0, -200, 200) == -200.0f);
    CHECK(rb::bendCents(8192, -200, 200) == 0.0f);
    for (int p = 0; p < 128; ++p)
    {
        CHECK(rb::repeatZone(p) == p / 32);
        CHECK(rb::userValue(p, 0.25f, 0.75f) == Catch::Approx(0.25 + 0.5 * p / 127.0));
        // PITCH / USER: lit up to the segment (UI-157); REPEAT: the zone's two LEDs (RIB-006)
        CHECK(rb::ledMask(Mode::Pitch, p, true) == static_cast<std::uint8_t>((2u << (p / 16)) - 1u));
        CHECK(rb::ledMask(Mode::Repeat, p, true) == static_cast<std::uint8_t>(3u << (2 * (p / 32))));
        CHECK(rb::ledMask(Mode::User1, p, false) == 0);
    }
    CHECK(rb::repeatQuarters(0) == 1.0);
    CHECK(rb::repeatQuarters(3) == 0.125);
    CHECK(rb::nextMode(Mode::User2) == Mode::Pitch);
}

TEST_CASE("ribbon.pitch: the held pad's track bends to exactly +200 cents at the top and springs back", "[ribbon]")
{
    auto r = makeRibbon();
    FakeMatrix m;
    press(r, 2, true);
    r.onStrip(127, true);
    idle(r);
    r.publish(m);
    CHECK(cents(m, 2) == Catch::Approx(200.0).margin(1e-3)); // ± 1 cent required; float exact to 1e-3
    CHECK(m.at(2, id("playback.pitch"))->kind == ModKind::Offset);
    CHECK(m.count() == 1); // only the held pad's track
    r.onStrip(0, true);
    idle(r);
    r.publish(m);
    CHECK(cents(m, 2) == Catch::Approx(-200.0).margin(1e-3));
    r.onStrip(64, true);
    idle(r);
    r.publish(m);
    CHECK(cents(m, 2) == 0.0f);
    r.onStrip(100, false); // release: spring back (RIB-002), the contribution is cleared
    idle(r);
    r.publish(m);
    CHECK(m.count() == 0);
    CHECK(r.ledMask() == 0);
}

TEST_CASE("ribbon.pitch: the range follows ribbon.pitchTop / pitchBottom; nothing without a held pad", "[ribbon]")
{
    auto r = makeRibbon();
    FakeMatrix m;
    REQUIRE(r.onParam(param("ribbon.pitchTop", 1200)));
    REQUIRE(r.onParam(param("ribbon.pitchBottom", -700)));
    r.onStrip(127, true);
    idle(r);
    r.publish(m);
    CHECK(m.count() == 0); // RIB-010: no pad held
    press(r, 0, true);     // strip first, then the pad: applies too
    r.publish(m);
    CHECK(cents(m, 0) == Catch::Approx(1200.0).margin(1e-2));
    r.onStrip(0, true);
    idle(r);
    r.publish(m);
    CHECK(cents(m, 0) == Catch::Approx(-700.0).margin(1e-2));
    press(r, 0, false); // the pad released while the strip is touched: its track returns to base
    r.publish(m);
    CHECK(m.count() == 0);
}

TEST_CASE("ribbon.hold: HOLD keeps the last value and LED, a second press releases, MODE cancels it", "[ribbon]")
{
    auto r = makeRibbon();
    FakeMatrix m;
    press(r, 5, true);
    REQUIRE(r.onParam(param("ribbon.hold", 1)));
    r.onStrip(110, true);
    idle(r);
    r.onStrip(110, false);
    press(r, 5, false); // the pad released too: the held value stays on its track (holdParameters)
    r.publish(m);
    const float held = core::ribbon::pitchCents(110, -200, 200);
    CHECK(cents(m, 5) == Catch::Approx(held).margin(1e-3));
    CHECK(r.ledMask() == core::ribbon::ledMask(Mode::Pitch, 110, true)); // the LED stays (RIB-005)
    press(r, 7, true); // a pad pressed later receives the held value too
    r.publish(m);
    CHECK(cents(m, 7) == Catch::Approx(held).margin(1e-3));
    REQUIRE(r.onParam(param("ribbon.hold", 0))); // second press: releaseParameters
    idle(r);
    r.publish(m);
    CHECK(cents(m, 5) == 0.0f);
    CHECK(cents(m, 7) == Catch::Approx(0.0)); // still held but the strip is not touched: base
    CHECK(m.count() == 0);
    CHECK(r.ledMask() == 0);

    // MODE change cancels HOLD (RIB-005)
    REQUIRE(r.onParam(param("ribbon.hold", 1)));
    r.onStrip(30, true);
    idle(r);
    r.onStrip(30, false);
    idle(r);
    r.publish(m);
    CHECK(m.count() == 1);
    CHECK(r.hold());
    REQUIRE(r.onParam(param("ribbon.mode", static_cast<float>(Mode::Repeat))));
    CHECK_FALSE(r.hold());
    CHECK(r.effectivePosition() == -1);
    idle(r);
    r.publish(m);
    CHECK(m.count() == 0);
    CHECK(r.ledMask() == 0);
}

TEST_CASE("ribbon.user: up to 8 items move together bottom → top as Overrides and release to base", "[ribbon]")
{
    auto r = makeRibbon();
    FakeMatrix m;
    assign(r, 0, 0, "env.velocity", 0.0f, 1.0f);
    assign(r, 0, 1, "fx1.gainTest.gain", 0.8f, 0.2f); // a falling item
    assign(r, 1, 0, "track.pan", 0.5f, 0.5f);         // USER2: not used in USER1
    REQUIRE(r.onParam(param("ribbon.mode", static_cast<float>(Mode::User1))));
    press(r, 3, true);
    press(r, 9, true);
    for (int p : {0, 31, 64, 100, 127})
    {
        r.onStrip(p, true);
        idle(r);
        r.publish(m);
        for (int t : {3, 9})
        {
            const auto* a = m.at(t, id("env.velocity"));
            const auto* c = m.at(t, id("fx1.gainTest.gain"));
            REQUIRE(a != nullptr);
            REQUIRE(c != nullptr);
            CHECK(a->kind == ModKind::Override);
            CHECK(a->value == Catch::Approx(p / 127.0).margin(1e-6));
            CHECK(c->value == Catch::Approx(0.8 - 0.6 * p / 127.0).margin(1e-6));
        }
        CHECK(m.at(3, id("track.pan")) == nullptr);
        CHECK(m.count() == 4);
    }
    r.onStrip(127, false);
    idle(r);
    r.publish(m);
    CHECK(m.count() == 0); // released: every target back to its base
    // An item deleted while touched is cleared, the others stay.
    r.onStrip(64, true);
    idle(r);
    r.publish(m);
    REQUIRE(r.onParam(param("ribbon.userTarget", -1, 1)));
    r.publish(m);
    CHECK(m.at(3, id("fx1.gainTest.gain")) == nullptr);
    CHECK(m.at(3, id("env.velocity")) != nullptr);
}

TEST_CASE("ribbon.apply: the preference picks the selected track or every track", "[ribbon]")
{
    auto r = makeRibbon();
    FakeMatrix m;
    REQUIRE(r.onParam(param("ribbon.applyTo", 1)));
    r.onStrip(127, true);
    engine::trigger::PadState pads;
    pads.selected = 11;
    r.observe({}, pads);
    r.publish(m);
    CHECK(m.count() == 1);
    CHECK(cents(m, 11) == Catch::Approx(200.0).margin(1e-3));
    REQUIRE(r.onParam(param("ribbon.applyTo", 2)));
    r.observe({}, pads);
    r.publish(m);
    CHECK(m.count() == 16);
}

TEST_CASE("ribbon.bend: incoming pitch bend drives PITCH mode only (RIB-007)", "[ribbon]")
{
    auto r = makeRibbon();
    FakeMatrix m;
    r.onPitchBend(4, 16383);
    r.publish(m);
    const auto* e = m.at(4, id("playback.pitch"), ModSource::PitchBend);
    REQUIRE(e != nullptr);
    CHECK(engine::modulation::pitchOffsetCents(e->value) == Catch::Approx(200.0).margin(1e-3));
    REQUIRE(r.onParam(param("ribbon.mode", static_cast<float>(Mode::Repeat))));
    r.publish(m);
    CHECK(m.at(4, id("playback.pitch"), ModSource::PitchBend) == nullptr); // leaving PITCH releases the bend
    r.onPitchBend(4, 0);
    r.publish(m);
    CHECK(m.count(ModSource::PitchBend) == 0); // ignored outside PITCH
}

TEST_CASE("ribbon.publish: unchanged output costs no matrix call; a refused set is retried", "[ribbon]")
{
    auto r = makeRibbon();
    FakeMatrix m;
    press(r, 1, true);
    r.onStrip(90, true);
    idle(r);
    r.publish(m);
    const int sets = m.sets;
    for (int i = 0; i < 10; ++i)
    {
        idle(r);
        r.publish(m);
    }
    CHECK(m.sets == sets);
    m.capacity = 1;
    press(r, 2, true);
    r.publish(m);
    CHECK(r.matrixRefusals() >= 1);
    m.capacity = 100;
    idle(r);
    r.publish(m);
    CHECK(m.count() == 2);
}

TEST_CASE("ribbon.repeat: hits sit on the 1/4 … 1/32 transport grid sample-accurately", "[ribbon]")
{
    for (double bpm : {120.0, 133.37, 87.5})
        for (int zone = 0; zone < 4; ++zone)
        {
            INFO("bpm " << bpm << " zone " << zone);
            auto r = makeRibbon();
            REQUIRE(r.onParam(param("ribbon.mode", static_cast<float>(Mode::Repeat))));
            r.onStrip(zone * 32 + 10, true);
            press(r, 6, true, 90);
            const double q = core::ribbon::repeatQuarters(zone);
            const double ppf = bpm / (60.0 * 44100.0);
            int hits = 0, expected = 0;
            for (long n = 0; n < 4000; ++n) // ≈ 5.8 s
            {
                const auto ctx = block(n, bpm, true);
                Hits h;
                r.collect(ctx, h);
                idle(r);
                // the grid lines in [p0, p1) and their frames (the sequencer's rounding)
                const long k0 = static_cast<long>(std::ceil((ctx.ppqStart - 1e-9) / q));
                for (long k = k0; k * q < ctx.ppqEnd - 1e-9; ++k)
                {
                    if (n == 0)
                        continue; // the pad's own hit is that note (pressed in block 0)
                    ++expected;
                    const double f = (k * q - ctx.ppqStart) / ppf + 1e-6;
                    REQUIRE(h.events.size() == 1);
                    CHECK(h.events[0].frame == static_cast<int>(f));
                    CHECK(h.events[0].track == 6);
                    CHECK(h.events[0].velocity == 90);
                    CHECK(h.events[0].source == engine::trigger::TriggerSource::Repeat);
                    CHECK(h.events[0].kind == engine::trigger::TriggerKind::Full);
                }
                hits += static_cast<int>(h.events.size());
            }
            CHECK(hits == expected);
            CHECK(expected >= static_cast<int>(5.8 * bpm / 60.0 / q) - 2);
        }
}

TEST_CASE("ribbon.repeat: stopped, it free-runs from the touch; pressure sets the volume; release stops", "[ribbon]")
{
    auto r = makeRibbon();
    REQUIRE(r.onParam(param("ribbon.mode", static_cast<float>(Mode::Repeat))));
    press(r, 0, true, 100);
    for (int i = 0; i < 100; ++i) // the pad held long before the touch
    {
        Hits h;
        r.collect(block(i, 120.0, false), h);
        idle(r);
        CHECK(h.events.empty());
    }
    r.onStrip(40, true); // 1/8 at 120 BPM = 11025 frames
    std::vector<double> at;
    for (long n = 0; n < 2000; ++n)
    {
        if (n == 700)
            r.onPressure(0, 30); // the repeat volume follows the pad's pressure (RIB-003)
        Hits h;
        r.collect(block(n, 120.0, false), h);
        idle(r);
        for (const auto& e : h.events)
        {
            at.push_back(static_cast<double>(n * engine::kBlockFrames + e.frame));
            CHECK(e.velocity == (n > 700 ? 30 : 100));
        }
    }
    REQUIRE(at.size() >= 10);
    CHECK(at[0] == 11025.0); // one interval after the touch
    for (std::size_t i = 1; i < at.size(); ++i)
        CHECK(std::fabs(at[i] - at[i - 1] - 11025.0) <= 1.0);
    // a tempo with a fractional interval does not drift (the phase keeps the fraction)
    auto s = makeRibbon();
    REQUIRE(s.onParam(param("ribbon.mode", static_cast<float>(Mode::Repeat))));
    press(s, 0, true);
    s.onStrip(127, true); // 1/32
    const double interval = 0.125 * 60.0 / 133.37 * 44100.0;
    std::vector<double> t2;
    for (long n = 0; n < 20000; ++n)
    {
        Hits h;
        s.collect(block(n, 133.37, false), h);
        idle(s);
        for (const auto& e : h.events)
            t2.push_back(static_cast<double>(n * engine::kBlockFrames + e.frame));
    }
    REQUIRE(t2.size() > 100);
    for (std::size_t i = 0; i < t2.size(); ++i)
        CHECK(std::fabs(t2[i] - std::floor(interval * static_cast<double>(i + 1))) <= 1.0);
    // release: nothing more (no HOLD)
    s.onStrip(127, false);
    Hits h;
    for (long n = 0; n < 1000; ++n)
        s.collect(block(n, 133.37, false), h);
    CHECK(h.events.empty());
    CHECK_FALSE(s.repeating());
}

TEST_CASE("ribbon.repeat: HOLD keeps the rate after the release while a pad is held; pad release stops", "[ribbon]")
{
    auto r = makeRibbon();
    REQUIRE(r.onParam(param("ribbon.mode", static_cast<float>(Mode::Repeat))));
    REQUIRE(r.onParam(param("ribbon.hold", 1)));
    press(r, 4, true);
    r.onStrip(80, true); // 1/16
    Hits h;
    r.collect(block(0, 120.0, false), h);
    r.onStrip(80, false);
    for (long n = 1; n < 1000; ++n)
    {
        r.collect(block(n, 120.0, false), h);
        idle(r);
    }
    CHECK(h.events.size() >= 10);
    CHECK(r.ledMask() == 0x30); // the 1/16 zone's two LEDs stay lit (RIB-005/006)
    press(r, 4, false);
    const auto before = h.events.size();
    for (long n = 0; n < 1000; ++n)
        r.collect(block(n, 120.0, false), h);
    CHECK(h.events.size() == before); // RIB-003: only while a pad is held
}

TEST_CASE("ribbon.repeat: a repeat replays the held pad's scale note (SCALE pad mode)", "[ribbon]")
{
    auto r = makeRibbon();
    REQUIRE(r.onParam(param("ribbon.mode", static_cast<float>(Mode::Repeat))));
    r.onStrip(80, true); // 1/16
    auto e = padEvent(2, true, 100);
    e.semitones = 7; // SCALE: the pad's note, a fifth up
    r.observe(std::span<const engine::trigger::TriggerEvent>(&e, 1), {});
    Hits h;
    for (long n = 0; n < 1000; ++n) // ≈ 11 16ths
    {
        r.collect(block(n, 120.0, true), h);
        idle(r);
    }
    REQUIRE(h.events.size() >= 5);
    for (const auto& ev : h.events)
        CHECK(ev.semitones == 7);
    // Another hit of the same track (PLAY: no offset) → the repeats follow it.
    e.semitones = 0;
    r.observe(std::span<const engine::trigger::TriggerEvent>(&e, 1), {});
    Hits after;
    for (long n = 1000; n < 2000; ++n)
    {
        r.collect(block(n, 120.0, true), after);
        idle(r);
    }
    REQUIRE_FALSE(after.events.empty());
    CHECK(after.events.back().semitones == 0);
}

TEST_CASE("ribbon.live: the TriggerNow strip stream and pad pressure codes", "[ribbon]")
{
    auto r = makeRibbon();
    engine::trigger::PadState pads;
    CHECK(r.onLive({0, 100, 1, core::ribbon::kStripPad}, pads));
    CHECK(r.contact());
    CHECK(r.position() == 100);
    CHECK(r.ledMask() == core::ribbon::ledMask(Mode::Pitch, 100, true));
    CHECK(r.onLive({0, 0, 0, core::ribbon::kStripPad}, pads));
    CHECK_FALSE(r.contact());
    CHECK(r.position() == 100); // a release keeps the last position
    CHECK(r.onLive({3, 50, 0, core::ribbon::kPressurePad}, pads));
    CHECK_FALSE(r.onLive({3, 100, 60, 3}, pads));  // a pad hit is the router's
    CHECK_FALSE(r.onLive({3, 100, 60, -1}, pads)); // a MIDI note too
    CHECK_FALSE(r.onParam(param("track.volume", 1)));
}
