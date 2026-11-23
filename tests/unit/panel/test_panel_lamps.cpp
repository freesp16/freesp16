// The panel's TX lamp frames (emulator panel_map.py E4/E7) and their schedule: on change, heartbeat ≤ 4 Hz, local
// echo until a LampModel flushes.
#include "io/controllers/ControlNames.h"
#include "io/controllers/LampRules.h"
#include "platform/imx6/panel/AnalogBoard.h"
#include "platform/imx6/panel/PanelCodec.h"
#include "platform/imx6/panel/PanelLamps.h"
#include "platform/imx6/panel/PanelRoles.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <set>

using namespace platform::imx6::panel;

namespace
{
// The emulator's decode_tx/decode_lamps for one lamp: RGB bytes of a channel, mono (lit, flag).
std::array<int, 3> rgbAt(const std::uint8_t* f, int channel)
{
    const int o = rgbOffset(channel);
    return {f[o], f[o + 1], f[o + 2]};
}
std::pair<int, int> monoAt(const std::uint8_t* f, int channel)
{
    const auto& m = kMono[channel];
    return {(f[m.byte] >> m.bit) & 1, m.flagByte == 0 ? -1 : (f[m.flagByte] >> m.bit) & 1};
}
} // namespace

TEST_CASE("panel lamps: a TX frame has header 2, the 0xaa marker, brightness 4 and a CRC")
{
    TxFrame f;
    f.finish();
    const auto* b = f.data();
    CHECK(b[0] == 2);
    CHECK(b[0x77] == 0xaa);
    CHECK(b[0x6e] == 4);
    CHECK(b[0x67] == 0); // never the standby flag
    CHECK(b[0x66] == 0); // nor HardMute
    CHECK(crcX25(b, 124) == static_cast<std::uint16_t>(b[124] | b[125] << 8));
}

TEST_CASE("panel lamps: pads and steps are RGB channels scaled to 0x4a, mono lamps bits with dim flags")
{
    TxFrame f;
    REQUIRE(f.setLamp(io::pad(1), 0xFF0000, 255));
    REQUIRE(f.setLamp(io::pad(16), 0x00FF80, 255));
    REQUIRE(f.setLamp(io::step(1), 0xFFFFFF, 128));
    REQUIRE(f.setLamp(io::step(16), 0x0000FF, 255));
    f.finish();
    CHECK(rgbAt(f.data(), 0x10) == std::array {74, 0, 0});
    CHECK(rgbAt(f.data(), 0x1f) == std::array {0, 74, 37});
    CHECK(rgbAt(f.data(), 0) == std::array {37, 37, 37});
    CHECK(rgbAt(f.data(), 15) == std::array {0, 0, 74});

    REQUIRE(f.setLamp(io::Control::Play, 0x00FF00, 255));     // mono 7, full
    REQUIRE(f.setLamp(io::Control::PadModeMute, 0xFFFFFF, io::kDimLevel)); // mono 1, dim flag
    REQUIRE(f.setLamp(io::Control::AfsOver, 0xFFB020, 255));  // mono 34, no flag
    REQUIRE(f.setLamp(io::barPositionLamp(2), 0x00FF00, 255)); // mono 31
    REQUIRE(f.setLamp(io::Control::StripLed8, 0xFFFFFF, 255)); // mono 22
    f.finish();
    CHECK(monoAt(f.data(), 7) == std::pair {1, 0});
    CHECK(monoAt(f.data(), 1) == std::pair {1, 1});
    CHECK(monoAt(f.data(), 34) == std::pair {1, -1});
    CHECK(monoAt(f.data(), 31) == std::pair {1, 0});
    CHECK(monoAt(f.data(), 22) == std::pair {1, -1});
    REQUIRE(f.setLamp(io::Control::Play, 0x00FF00, 0)); // off clears bit and flag
    f.finish();
    CHECK(monoAt(f.data(), 7) == std::pair {0, 0});

    CHECK_FALSE(f.setLamp(io::Control::Shift, 0xFFFFFF, 255)); // SHIFT has no lamp
    CHECK_FALSE(f.setLamp(io::knob(1), 0xFFFFFF, 255));
}

TEST_CASE("panel lamps: every lamp control of the logical map has a channel, and no two share one")
{
    TxFrame f;
    std::set<std::pair<bool, int>> used;
    for (const auto c : io::lampControls())
    {
        INFO(io::lampName(c));
        CHECK(f.setLamp(c, 0xFFFFFF, 255));
    }
    for (const auto& l : kLamps)
        CHECK(used.insert({l.rgb, l.channel}).second);
    CHECK(used.size() == io::lampControls().size());
}

TEST_CASE("panel lamps: the bus sends on change (coalesced) and resends as a 4 Hz heartbeat")
{
    auto bus = std::make_shared<LampBus>();
    PanelLamps lamps(bus);
    TxFrame out;
    double wait = 0;
    REQUIRE(bus->take(1000, out, wait)); // the initial frame
    CHECK_FALSE(bus->take(1001, out, wait));
    CHECK(wait <= LampBus::kHeartbeatMs);

    lamps.setLamp(io::Control::Play, 0x00FF00, 255);
    lamps.flush();
    CHECK(bus->external());
    CHECK_FALSE(bus->take(1005, out, wait)); // within kMinGapMs of the last send
    CHECK(wait <= LampBus::kMinGapMs);
    REQUIRE(bus->take(1011, out, wait));
    CHECK(monoAt(out.data(), 7).first == 1);

    lamps.flush(); // same frame: no change
    CHECK_FALSE(bus->take(1100, out, wait));
    REQUIRE(bus->take(1011 + LampBus::kHeartbeatMs, out, wait)); // heartbeat
    int sends = 0;
    for (double t = 1300; t < 2300; t += 5)
        sends += bus->take(t, out, wait) ? 1 : 0;
    CHECK(sends >= 3);
    CHECK(sends <= 4); // ≤ 4 Hz while nothing changes
}

TEST_CASE("panel lamps: the local echo flashes pads, lights held keys and the strip until a LampModel flushes")
{
    auto bus = std::make_shared<LampBus>();
    TxFrame out;
    double wait = 0;
    bus->take(0, out, wait);
    CHECK(monoAt(out.data(), 12).first == 1); // PITCH mode indicator (default strip mode)

    bus->echo({io::ControlEvent::Kind::Press, io::pad(5), 100, io::kSourcePanel, 100});
    REQUIRE(bus->take(100, out, wait));
    CHECK(rgbAt(out.data(), 0x10 + 4) == std::array {74, 74, 74}); // white
    CHECK(wait <= LampBus::kEchoTickMs);
    REQUIRE(bus->take(600, out, wait));
    const auto half = rgbAt(out.data(), 0x14);
    CHECK((half[0] > 0 && half[0] < 74));
    bus->take(1200, out, wait);
    CHECK(rgbAt(out.data(), 0x14) == std::array {0, 0, 0}); // faded out after ~1 s

    bus->echo({io::ControlEvent::Kind::Press, io::Control::Play, 0, io::kSourcePanel, 1300});
    bus->echo({io::ControlEvent::Kind::Touch, io::Control::Strip, 127, io::kSourceStrip, 1300});
    REQUIRE(bus->take(1300, out, wait));
    CHECK(monoAt(out.data(), 7).first == 1);
    for (int ch = 15; ch <= 22; ++ch)
        CHECK(monoAt(out.data(), ch).first == 1); // PITCH: lit up to the touched segment
    bus->echo({io::ControlEvent::Kind::Release, io::Control::Strip, 127, io::kSourceStrip, 1400});
    bus->take(1400, out, wait);
    CHECK(monoAt(out.data(), 15).first == 0);

    PanelLamps lamps(bus);
    lamps.flush(); // the LampModel takes over: an all-dark frame
    bus->take(1500, out, wait);
    CHECK(monoAt(out.data(), 7).first == 0);
    bus->echo({io::ControlEvent::Kind::Press, io::Control::Play, 0, io::kSourcePanel, 1600});
    CHECK_FALSE(bus->take(1600, out, wait)); // the echo is off for good
}

TEST_CASE("analog board: the HP CUTOFF DAC code stays inside the factory range 4…3276, rising with the pot")
{
    CHECK(AnalogBoard::dacCode(0) == 4u);
    CHECK(AnalogBoard::dacCode(1023) == 3276u);
    CHECK(AnalogBoard::dacCode(-5) == 4u);
    CHECK(AnalogBoard::dacCode(5000) == 3276u);
    unsigned last = 0;
    for (int i = 0; i <= 1023; ++i)
    {
        const unsigned c = AnalogBoard::dacCode(i);
        INFO("pot " << i);
        CHECK(c >= last);
        CHECK(c <= 3276u);
        last = c;
    }
    // A sweep that spends most of the pot in the low range (an even spread in Hz, not in volts).
    CHECK(AnalogBoard::dacCode(512) < 3276u / 6);
}

TEST_CASE("analog board: the select line SEL_xDSF/THRU is active low (0 = the filter in, 1 = THRU)")
{
    STATIC_CHECK(AnalogBoard::selectLevel(true) == 0);
    STATIC_CHECK(AnalogBoard::selectLevel(false) == 1);
    STATIC_CHECK(AnalogBoard::kSelectLine == 133);
    STATIC_CHECK(AnalogBoard::kLimitLine == 134);
}

TEST_CASE("analog board: the DAC driver's write returns 0 on success (the vendor's SPI contract), 2 elsewhere")
{
    STATIC_CHECK(AnalogBoard::dacWriteOk(0));
    STATIC_CHECK(AnalogBoard::dacWriteOk(2));
    STATIC_CHECK_FALSE(AnalogBoard::dacWriteOk(-1));
    STATIC_CHECK_FALSE(AnalogBoard::dacWriteOk(1));
}

TEST_CASE("panel lamps: HardMute, then standby sends one dark frame with the 0x67 flag, at once and on the heartbeat")
{
    auto bus = std::make_shared<LampBus>();
    PanelLamps lamps(bus);
    TxFrame out;
    double wait = 0;
    CHECK_FALSE(bus->enterStandby()); // no sender attached: nothing would carry the flag
    CHECK_FALSE(bus->standby());

    int woken = 0;
    bus->setWake([](void* n) { ++*static_cast<int*>(n); }, &woken);
    lamps.setLamp(io::Control::Play, 0x00FF00, 255);
    lamps.flush();
    REQUIRE(bus->take(1000, out, wait));
    CHECK_FALSE(out.standby());
    CHECK(monoAt(out.data(), 7).first == 1);

    // The power key: HardMute (0x66 bit 0) on every frame from now on, the lamps unchanged, no standby flag yet.
    int before = woken;
    bus->hardMute();
    CHECK(woken == before + 1);
    REQUIRE(bus->take(1011, out, wait));
    CHECK(out.data()[0x66] == 1);
    CHECK_FALSE(out.standby());
    CHECK(monoAt(out.data(), 7).first == 1);
    CHECK(crcX25(out.data(), 124) == static_cast<std::uint16_t>(out.data()[124] | out.data()[125] << 8));
    REQUIRE(bus->take(1011 + LampBus::kHeartbeatMs, out, wait)); // the heartbeat carries it too
    CHECK(out.data()[0x66] == 1);

    before = woken;
    REQUIRE(bus->enterStandby());
    CHECK(bus->standby());
    CHECK(woken == before + 1); // the sender sends it without waiting for the heartbeat
    REQUIRE(bus->take(1300, out, wait));
    const auto* b = out.data();
    CHECK(out.standby());
    CHECK(b[0x67] == 1);
    CHECK(b[0x66] == 1); // HardMute stays
    CHECK(b[0x68] == 0); // "standby controller absent" stays clear
    CHECK(b[0] == 2);
    CHECK(b[0x77] == 0xaa);
    CHECK(crcX25(b, 124) == static_cast<std::uint16_t>(b[124] | b[125] << 8));
    CHECK(monoAt(b, 7).first == 0); // every lamp dark
    for (int ch = 0; ch < 32; ++ch)
        CHECK(rgbAt(b, ch) == std::array {0, 0, 0});

    // A late LampModel flush or input echo never replaces it; the heartbeat keeps sending it.
    lamps.flush();
    bus->echo({io::ControlEvent::Kind::Press, io::pad(1), 100, io::kSourcePanel, 1310});
    CHECK_FALSE(bus->take(1320, out, wait));
    REQUIRE(bus->take(1300 + LampBus::kHeartbeatMs, out, wait));
    CHECK(out.standby());
    CHECK(monoAt(out.data(), 7).first == 0);
    CHECK(rgbAt(out.data(), 0x10) == std::array {0, 0, 0});
}
