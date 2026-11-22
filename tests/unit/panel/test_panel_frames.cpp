// The panel's RX frames → logical ControlEvents: synthetic frames built
// from the emulator's documented layout, every control of control-surface.md §2.1 the panel carries.
#include "tests/unit/panel/PanelTestFrames.h"

#include "io/controllers/ControlNames.h"
#include "platform/imx6/panel/PanelCodec.h"
#include "platform/imx6/panel/PanelRoles.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using platform::imx6::panel::EventBatch;
using platform::imx6::panel::PanelDecoder;
using Kind = io::ControlEvent::Kind;

namespace
{
struct Sink final : engine::TriggerSink
{
    std::vector<engine::TriggerNow> hits;
    bool push(const engine::TriggerNow& t) noexcept override
    {
        hits.push_back(t);
        return true;
    }
};

std::vector<io::ControlEvent> run(PanelDecoder& d, paneltest::RxFrame& f, double t = 0)
{
    EventBatch batch;
    REQUIRE(platform::imx6::panel::rxFrameOk(f.finish()));
    d.decode(f.b.data(), t, batch);
    return {batch.events.begin(), batch.events.begin() + batch.count};
}

// A decoder past its first frame (the initial pot values reported and consumed).
PanelDecoder primed(paneltest::RxFrame& f)
{
    PanelDecoder d;
    run(d, f);
    return d;
}
} // namespace

TEST_CASE("panel frames: CRC-16/X-25 and header gate every frame")
{
    paneltest::RxFrame f;
    REQUIRE(platform::imx6::panel::rxFrameOk(f.finish()));
    f.b[10] ^= 1;
    CHECK_FALSE(platform::imx6::panel::rxFrameOk(f.b.data()));
    f.b[10] ^= 1;
    f.b[0] = 2;
    CHECK_FALSE(platform::imx6::panel::rxFrameOk(f.finish()));
    // CRC-16/X-25 check value of "123456789".
    const std::uint8_t text[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(platform::imx6::panel::crcX25(text, 9) == 0x906E);
}

TEST_CASE("panel frames: every button role produces Press and Release of its control")
{
    for (const auto& role : platform::imx6::panel::kButtons)
    {
        INFO(io::nameOf(role.control) << " at byte " << int(role.byte) << " bit " << int(role.bit));
        paneltest::RxFrame f;
        auto d = primed(f);
        f.button(role.byte, role.bit, true);
        auto ev = run(d, f, 10);
        REQUIRE(ev.size() == 1);
        CHECK(ev[0].kind == Kind::Press);
        CHECK(ev[0].control == role.control);
        CHECK(ev[0].source == io::kSourcePanel);
        CHECK(ev[0].timeMs == 10);
        CHECK(run(d, f).empty()); // levels: no repeat while held
        f.button(role.byte, role.bit, false);
        ev = run(d, f);
        REQUIRE(ev.size() == 1);
        CHECK(ev[0].kind == Kind::Release);
        CHECK(ev[0].control == role.control);
    }
}

TEST_CASE("panel frames: system keys and inert bits produce nothing")
{
    paneltest::RxFrame f;
    auto d = primed(f);
    for (auto [byte, bit] : {std::pair {5, 6}, {5, 4}, {1, 0}, {5, 7}})
        f.button(byte, bit, true);
    CHECK(run(d, f).empty());
}

TEST_CASE("panel frames: chords report every key in one frame")
{
    paneltest::RxFrame f;
    auto d = primed(f);
    f.button(2, 0, true); // shift
    f.button(2, 7, true); // pattern
    const auto ev = run(d, f);
    REQUIRE(ev.size() == 2);
    CHECK(ev[0].control == io::Control::Shift);
    CHECK(ev[1].control == io::Control::Pattern);
}

TEST_CASE("panel frames: pads hit with the emulator's velocities, go to the trigger sink first, then release")
{
    Sink sink;
    for (int velocity : {1, 2, 50, 100, 110, 126, 127})
        for (int n : {1, 5, 16})
        {
            INFO("pad " << n << " velocity " << velocity);
            paneltest::RxFrame f;
            auto d = primed(f);
            d.setTriggers(&sink);
            sink.hits.clear();
            f.pad(n, io::PadCurve::emulatorWord(velocity), 512);
            auto ev = run(d, f);
            REQUIRE(ev.size() == 1);
            CHECK(ev[0].kind == Kind::Press);
            CHECK(ev[0].control == io::pad(n));
            CHECK(ev[0].value == velocity);
            REQUIRE(sink.hits.size() == 1);
            CHECK(sink.hits[0].track == n - 1);
            CHECK(sink.hits[0].pad == n - 1);
            CHECK(sink.hits[0].velocity == velocity);
            CHECK(run(d, f).empty()); // held, same pressure
            f.pad(n, 0, 0);
            ev = run(d, f);
            REQUIRE(ev.size() == 1);
            CHECK(ev[0].kind == Kind::Release);
            CHECK(ev[0].control == io::pad(n));
            CHECK(sink.hits.size() == 1);
        }
}

TEST_CASE("panel frames: a pad needs contact and a pressure above the threshold")
{
    paneltest::RxFrame f;
    auto d = primed(f);
    f.pad(3, 800, 59); // no contact
    CHECK(run(d, f).empty());
    f.pad(3, 168, 512); // below the hit threshold (169)
    CHECK(run(d, f).empty());
    f.pad(3, 169, 512);
    const auto ev = run(d, f);
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].value == 1);
}

TEST_CASE("panel frames: pressure while held is PadPressure 0-127 (UI-115)")
{
    paneltest::RxFrame f;
    auto d = primed(f);
    f.pad(7, 300, 512);
    REQUIRE(run(d, f).size() == 1);
    f.pad(7, 874, 512);
    auto ev = run(d, f);
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].kind == Kind::Pressure);
    CHECK(ev[0].control == io::pad(7));
    CHECK(ev[0].value == 127);
    f.pad(7, 100, 512); // above the release word (threshold / 2)
    ev = run(d, f);
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].kind == Kind::Pressure);
    CHECK(ev[0].value == 0);
}

TEST_CASE("panel frames: encoder and selector counters are cumulative int16 with wrap-around")
{
    paneltest::RxFrame f;
    f.counter(0x30 + 4, 0x7ffe); // knob 3 starts anywhere: the first frame is the reference
    f.counter(0x3c, 0xffff);
    auto d = primed(f);
    f.counter(0x30 + 4, 0x8001); // +3 across the int16 wrap
    f.counter(0x3c, 0x0001);     // +2 across zero
    auto ev = run(d, f);
    REQUIRE(ev.size() == 2);
    CHECK(ev[0].kind == Kind::Turn);
    CHECK(ev[0].control == io::knob(3));
    CHECK(ev[0].value == 3);
    CHECK(ev[1].control == io::Control::Selector);
    CHECK(ev[1].value == 2);
    for (int k = 1; k <= 6; ++k)
    {
        f.counter(0x30 + 2 * (k - 1), (k == 3 ? 0x8001 : 0) - k);
        ev = run(d, f);
        REQUIRE(ev.size() == 1);
        CHECK(ev[0].control == io::knob(k));
        CHECK(ev[0].value == -k);
    }
}

TEST_CASE("panel frames: analog pots are absolute 0-1023, clockwise more, with a deadband")
{
    paneltest::RxFrame f;
    PanelDecoder d;
    auto ev = run(d, f); // first frame: every pot reports its position (UI-116 needs it)
    REQUIRE(ev.size() == 3);
    CHECK(ev[0].control == io::Control::AfsDrive);
    CHECK(ev[0].value == 0);
    CHECK(ev[1].control == io::Control::MainVolume);
    CHECK(ev[1].value == 1023);
    CHECK(ev[2].control == io::Control::AfsHpCutoff);
    CHECK(ev[2].value == 0);
    f.setAnalog(38, 1023 - 512); // DRIVE half (inverted word)
    f.setAnalog(40, 1022);       // 1 LSB: inside the deadband
    ev = run(d, f);
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].kind == Kind::Value);
    CHECK(ev[0].control == io::Control::AfsDrive);
    CHECK(ev[0].value == 512);
    f.setAnalog(40, 1023); // back to the end stop: always reported... unless it is the stored value
    CHECK(run(d, f).empty());
    f.setAnalog(40, 500);
    f.setAnalog(42, 1023 - 1023); // HP CUTOFF full
    ev = run(d, f);
    REQUIRE(ev.size() == 2);
    CHECK(ev[0].value == 500);
    CHECK(ev[1].control == io::Control::AfsHpCutoff);
    CHECK(ev[1].value == 1023);
}

TEST_CASE("panel frames: LCD touch is Touch 1/2/0 on Screen in 800 x 480 points after a stable contact")
{
    // The emulator's inverse calibration (panel_map.screen_to_touch_raw) of a few screen points.
    auto raw = [](int x, int y) {
        const double rx = 940 - (x - 40) * (940.0 - 72.0) / 720.0, ry = 87 + (y - 29) * (866.0 - 87.0) / 405.0;
        return std::pair {static_cast<int>(rx + 0.5), static_cast<int>(ry + 0.5)};
    };
    paneltest::RxFrame f;
    auto d = primed(f);
    auto [rx, ry] = raw(400, 240);
    f.touch(rx, ry);
    CHECK(run(d, f).empty()); // 1st and 2nd stable sample
    CHECK(run(d, f).empty());
    auto ev = run(d, f);
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].kind == Kind::Touch);
    CHECK(ev[0].control == io::Control::Screen);
    CHECK(ev[0].value == 1);
    CHECK(ev[0].x == 400);
    CHECK(ev[0].y == 240);
    CHECK(run(d, f).empty()); // no motion, no drag
    std::tie(rx, ry) = raw(410, 250);
    f.touch(rx, ry);
    ev = run(d, f);
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].value == 2);
    CHECK(ev[0].x == 410);
    CHECK(ev[0].y == 250);
    f.touch(0, 0); // release: idle (0, 0) is outside the contact rectangle
    CHECK(run(d, f).empty());
    ev = run(d, f);
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].value == 0);
    CHECK(ev[0].x == 410);

    // Corners map inside the screen; a single in-bounds glitch never touches.
    int x, y;
    PanelDecoder::touchToScreen({}, 940, 87, x, y);
    CHECK((x == 40 && y == 29));
    PanelDecoder::touchToScreen({}, 72, 866, x, y);
    CHECK((x == 760 && y == 434));
    PanelDecoder::touchToScreen({}, 1023, 1023, x, y);
    CHECK((x >= 0 && x < 800 && y >= 0 && y < 480));
    f.touch(500, 500);
    run(d, f);
    f.touch(0, 0);
    run(d, f);
    run(d, f);
    CHECK(run(d, f).empty());
}

TEST_CASE("panel frames: the first frame reports held buttons and pads but no counter motion")
{
    paneltest::RxFrame f;
    f.button(2, 3, true); // play held at start
    f.counter(0x30, 1234);
    f.pad(2, io::PadCurve::emulatorWord(64), 512);
    PanelDecoder d;
    const auto ev = run(d, f);
    int presses = 0, turns = 0;
    for (const auto& e : ev)
    {
        presses += e.kind == Kind::Press;
        turns += e.kind == Kind::Turn;
    }
    CHECK(presses == 2);
    CHECK(turns == 0);
}
