// Synthetic RX frames built from the documented layout (sp16-emulator panel_map.py E1-E11), never captured traffic.
#pragma once

#include "platform/imx6/panel/PanelCodec.h"

#include <array>
#include <cstdint>

namespace paneltest
{
struct RxFrame
{
    std::array<std::uint8_t, 128> b {};
    RxFrame()
    {
        b[0] = 1;
        setAnalog(38, 1023); // DRIVE at minimum (inverted word), MAIN OUT full, HP CUTOFF at minimum
        setAnalog(40, 1023);
        setAnalog(42, 1023);
    }
    void word(int offset, int v)
    {
        b[static_cast<std::size_t>(offset)] = static_cast<std::uint8_t>((v >> 8) & 0xff);
        b[static_cast<std::size_t>(offset + 1)] = static_cast<std::uint8_t>(v & 0xff);
    }
    void setAnalog(int offset, int v) { word(offset, v); }
    void button(int byte, int bit, bool on)
    {
        auto& x = b[static_cast<std::size_t>(byte)];
        x = static_cast<std::uint8_t>(on ? x | (1u << bit) : x & ~(1u << bit));
    }
    void pad(int n1, int pressure, int contact)
    {
        word(0x06 + 2 * (n1 - 1), pressure);
        word(0x3f + 2 * (n1 - 1), contact);
    }
    void counter(int offset, int v) { word(offset, v & 0xffff); }
    void touch(int rx, int ry)
    {
        word(0x2c, rx);
        word(0x2e, ry);
    }
    const std::uint8_t* finish()
    {
        const auto crc = platform::imx6::panel::crcX25(b.data(), 124);
        b[124] = static_cast<std::uint8_t>(crc & 0xff);
        b[125] = static_cast<std::uint8_t>(crc >> 8);
        return b.data();
    }
};
} // namespace paneltest
