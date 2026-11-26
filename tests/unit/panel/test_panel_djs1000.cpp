// The DJS-1000's panel: the same frame as the SP-16's with its own layout (PanelRoles.h, namespace djs1000). Synthetic
// frames built from that layout, never captured traffic.
#include "io/controllers/ControlNames.h"
#include "io/controllers/LampRules.h"
#include "platform/imx6/panel/PanelCodec.h"
#include "platform/imx6/panel/PanelLamps.h"
#include "platform/imx6/panel/PanelRoles.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

using namespace platform::imx6::panel;
using Kind = io::ControlEvent::Kind;

namespace
{
// An RX frame of the DJS-1000: little-endian words.
struct DjsFrame
{
    std::array<std::uint8_t, 128> b {};
    DjsFrame() { b[0] = 1; }
    void word(int offset, int v)
    {
        b[static_cast<std::size_t>(offset)] = static_cast<std::uint8_t>(v & 0xff);
        b[static_cast<std::size_t>(offset + 1)] = static_cast<std::uint8_t>((v >> 8) & 0xff);
    }
    void button(int byte, int bit, bool on)
    {
        auto& x = b[static_cast<std::size_t>(byte)];
        x = static_cast<std::uint8_t>(on ? x | (1u << bit) : x & ~(1u << bit));
    }
    const std::uint8_t* finish()
    {
        const auto crc = crcX25(b.data(), kCrcOffset);
        b[kCrcOffset] = static_cast<std::uint8_t>(crc & 0xff);
        b[kCrcOffset + 1] = static_cast<std::uint8_t>(crc >> 8);
        return b.data();
    }
};

std::vector<io::ControlEvent> run(PanelDecoder& d, DjsFrame& f, double t = 0)
{
    EventBatch batch;
    REQUIRE(rxFrameOk(f.finish()));
    d.decode(f.b.data(), t, batch);
    return {batch.events.begin(), batch.events.begin() + batch.count};
}

PanelDecoder djsDecoder()
{
    DecoderConfig c;
    c.unit = Unit::Djs1000;
    return PanelDecoder(c);
}

bool has(const std::vector<io::ControlEvent>& events, Kind kind, io::Control c, int value)
{
    for (const auto& e : events)
        if (e.kind == kind && e.control == c && e.value == value)
            return true;
    return false;
}
} // namespace

TEST_CASE("DJS-1000 panel: every button role produces Press and Release of its control")
{
    for (const auto& role : djs1000::kButtons)
    {
        INFO(io::nameOf(role.control));
        auto d = djsDecoder();
        DjsFrame f;
        run(d, f);
        f.button(role.byte, role.bit, true);
        CHECK(has(run(d, f), Kind::Press, role.control, 0));
        f.button(role.byte, role.bit, false);
        CHECK(has(run(d, f), Kind::Release, role.control, 0));
    }
}

TEST_CASE("DJS-1000 panel: the counters are little-endian, the selector at 0x14")
{
    auto d = djsDecoder();
    DjsFrame f;
    f.word(0x14, 0xfffe);
    f.word(0x08, 10);
    run(d, f); // the reference
    f.word(0x14, 0x0001); // +3 across the wrap
    f.word(0x08, 8);
    const auto e = run(d, f);
    CHECK(has(e, Kind::Turn, io::Control::Selector, 3));
    CHECK(has(e, Kind::Turn, io::Control::Knob1, -2));
}

TEST_CASE("DJS-1000 panel: the tempo fader, FX LEVEL and PHONES LEVEL are absolute values")
{
    auto d = djsDecoder();
    DjsFrame f;
    f.word(0x58, 600);
    f.word(0x5a, 0);
    f.word(0x5c, 1023);
    const auto e = run(d, f);
    CHECK(has(e, Kind::Value, io::Control::TempoFader, 600));
    CHECK(has(e, Kind::Value, io::Control::FxLevel, 0));
    CHECK(has(e, Kind::Value, io::Control::PhonesLevel, 1023));
}

TEST_CASE("DJS-1000 panel: a pad hit reads the force at 0x16 and the contact at 0x36")
{
    auto d = djsDecoder();
    DjsFrame f;
    run(d, f);
    f.word(0x16 + 2 * 5, 800);
    f.word(0x36 + 2 * 5, 800);
    const auto hit = run(d, f, 10);
    REQUIRE(! hit.empty());
    CHECK(hit.front().kind == Kind::Press);
    CHECK(hit.front().control == io::pad(6));
    CHECK(hit.front().value > 0);
    f.word(0x16 + 2 * 5, 0);
    f.word(0x36 + 2 * 5, 0);
    CHECK(has(run(d, f, 20), Kind::Release, io::pad(6), 0));
}

TEST_CASE("DJS-1000 lamps: two bits a lamp, brightness at 0x6d, no SP-16 marker")
{
    TxFrame f(Unit::Djs1000);
    CHECK(f.setLamp(io::Control::Play, io::lampcolour::green, 255)); // lamp 0x16: byte 0x68, bits 0-1
    CHECK(f.setLamp(io::Control::Sync, io::lampcolour::white, 100)); // lamp 0x1b: byte 0x6a, bits 0-1, dim
    CHECK_FALSE(f.setLamp(io::Control::PadModeTrack, io::lampcolour::white, 255)); // no TRACK key
    f.finish();
    const auto* b = f.data();
    CHECK((b[0x68] & 3) == 1);
    CHECK((b[0x6a] & 3) == 3);
    CHECK(b[0x6d] == 4);
    CHECK(b[0x77] == 0);
    CHECK(crcX25(b, kCrcOffset) == static_cast<std::uint16_t>(b[kCrcOffset] | b[kCrcOffset + 1] << 8));
    f.setLamp(io::Control::Play, 0, 0);
    CHECK((f.data()[0x68] & 3) == 0);
}

TEST_CASE("DJS-1000 lamps: standby and mute are bits 1 and 0 of 0x6c")
{
    TxFrame f(Unit::Djs1000);
    f.setHardMute(true);
    f.setStandby(true);
    f.finish();
    CHECK(f.data()[0x6c] == 0x03);
    CHECK(f.standby());
    CHECK(f.data()[0x66] == 0);
    CHECK(f.data()[0x67] == 0);
}

TEST_CASE("DJS-1000 lamps: the bus keeps the outputs muted until the audio runs, and the start-up flag until an answer")
{
    LampBus bus(Unit::Djs1000);
    TxFrame out(Unit::Djs1000);
    double wait = 0;
    REQUIRE(bus.take(0, out, wait));
    CHECK(out.data()[0x6c] == (djs1000::kFlagMute | djs1000::kFlagStartup));
    bus.answered();
    REQUIRE(bus.take(1000, out, wait));
    CHECK(out.data()[0x6c] == djs1000::kFlagMute);
    bus.setOutputsLive(true);
    REQUIRE(bus.take(2000, out, wait));
    CHECK(out.data()[0x6c] == 0);
    CHECK(crcX25(out.data(), kCrcOffset)
          == static_cast<std::uint16_t>(out.data()[kCrcOffset] | out.data()[kCrcOffset + 1] << 8));
    bus.setOutputsLive(false); // the audio stopped
    REQUIRE(bus.take(3000, out, wait));
    CHECK(out.data()[0x6c] == djs1000::kFlagMute);
}

TEST_CASE("DJS-1000 lamps: the SP-16's bus never writes the DJS-1000's flags")
{
    LampBus bus;
    TxFrame out;
    double wait = 0;
    REQUIRE(bus.take(0, out, wait));
    CHECK(out.data()[0x6c] == 0);
    bus.setOutputsLive(false);
    REQUIRE(bus.take(1000, out, wait));
    CHECK(out.data()[0x6c] == 0);
    CHECK(out.data()[0x66] == 0);
}
