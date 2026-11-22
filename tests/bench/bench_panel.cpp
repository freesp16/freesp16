// Panel input/lamp cost (budget: ≤ 1 % of one Cortex-A9 core at the panel frame rate).
// The panel driver is set to a 3 ms interval (≈ 333 frames/s, the worst case; the emulator sends every 10 ms), so 1 %
// of a 1 GHz core is 30 000 cycles = 30 µs per frame on the device. The host run here is relative only: it asserts a
// tenth of that (3 µs per frame: decode + events + one TX frame render), assuming the host core is ≤ 10× an A9 core.
// The device number is open until measured on the hardware.
#include "platform/imx6/panel/AnalogBoard.cpp"
#include "platform/imx6/panel/PanelCodec.cpp"
#include "platform/imx6/panel/PanelLamps.cpp"
#include "platform/imx6/strip/StripCodec.cpp"

#include "io/controllers/LampRules.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <vector>

using namespace platform::imx6::panel;

namespace
{
struct NullSink final : engine::TriggerSink
{
    long n = 0;
    bool push(const engine::TriggerNow&) noexcept override { return ++n > 0; }
};

std::vector<std::array<std::uint8_t, 128>> frames()
{
    // A busy stream: pads hit and released, two buttons toggling, a knob and the selector turning, a pot moving,
    // the LCD touched — every frame different.
    std::vector<std::array<std::uint8_t, 128>> out;
    for (int i = 0; i < 256; ++i)
    {
        std::array<std::uint8_t, 128> f {};
        f[0] = 1;
        auto word = [&](int o, int v) {
            f[static_cast<std::size_t>(o)] = static_cast<std::uint8_t>(v >> 8);
            f[static_cast<std::size_t>(o + 1)] = static_cast<std::uint8_t>(v);
        };
        const int pad = i % 16;
        if ((i / 16) % 2 == 0)
        {
            word(0x06 + 2 * pad, io::PadCurve::emulatorWord(1 + i % 127));
            word(0x3f + 2 * pad, 512);
        }
        f[2] = static_cast<std::uint8_t>(i & 0x09);
        f[3] = static_cast<std::uint8_t>((i >> 1) & 0x81);
        word(0x30, i);
        word(0x3c, -i);
        word(0x28, (i * 7) % 1024);
        word(0x2c, i % 64 < 40 ? 400 + i % 8 : 0);
        word(0x2e, i % 64 < 40 ? 500 : 0);
        const auto crc = crcX25(f.data(), 124);
        f[124] = static_cast<std::uint8_t>(crc);
        f[125] = static_cast<std::uint8_t>(crc >> 8);
        out.push_back(f);
    }
    return out;
}
} // namespace

TEST_CASE("bench panel: RX decode + TX lamp frame per panel frame within budget")
{
    const auto input = frames();
    PanelDecoder decoder;
    NullSink sink;
    decoder.setTriggers(&sink);
    auto bus = std::make_shared<LampBus>();
    EventBatch batch;
    TxFrame out;
    double wait = 0;
    long events = 0;
    constexpr int kFrames = 200000;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kFrames; ++i)
    {
        const auto& f = input[static_cast<std::size_t>(i) & 255];
        if (! rxFrameOk(f.data()))
            FAIL("bad frame");
        batch.count = 0;
        decoder.decode(f.data(), i * 3.0, batch);
        events += batch.count;
        for (int k = 0; k < batch.count; ++k)
            bus->echo(batch.events[static_cast<std::size_t>(k)]);
        bus->take(i * 3.0, out, wait);
    }
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / kFrames;
    std::printf("BENCH panel_frame ns_per_frame=%.1f events_per_frame=%.2f (device budget 30000 ns at 333 Hz)\n", ns,
                static_cast<double>(events) / kFrames);
    CHECK(events > kFrames); // the stream is busy
    CHECK(ns < 3000.0);

    // Decode alone (no lamp echo): the steady-state cost once a LampModel drives the lamps.
    const auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < kFrames; ++i)
    {
        batch.count = 0;
        const auto& f = input[static_cast<std::size_t>(i) & 255];
        (void) rxFrameOk(f.data());
        decoder.decode(f.data(), i * 3.0, batch);
    }
    const double nsDecode =
        std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t1).count() / kFrames;
    std::printf("BENCH panel_decode ns_per_frame=%.1f\n", nsDecode);
    CHECK(nsDecode < 1000.0);
}
