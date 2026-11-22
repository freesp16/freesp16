// The touch strip's CDC frames (/dev/cdc_spi2.0), device-independent. Layout (sp16-emulator panel_map.py E5, not
// firmware code): 4-byte frames, value = (0x7f − b0) | b1 << 8; position = value & 0x7f (0 bottom … 127
// top), contact = bit 15 (b1 bit 7). Frames are sent on change.
//
// Events (Control::Strip, source kSourceStrip; control-surface.md UI-110 StripTouch / AbsoluteValue / StripRelease):
// Kind::Touch value = position when the finger lands, Kind::Value value = position while it moves, Kind::Release
// value = the last position when it leaves. The strip's musical behaviour (modes, HOLD) is core/util/ribbon's.
#pragma once

#include "io/controllers/ControlSurface.h"

#include <cstdint>

namespace platform::imx6::strip
{
constexpr int kFrameBytes = 4;

struct StripSample
{
    int position; // 0…127
    bool contact;
};
constexpr StripSample decodeFrame(const std::uint8_t* f) noexcept
{
    const unsigned word = (static_cast<unsigned>(0x7f - f[0]) & 0xffu) | static_cast<unsigned>(f[1]) << 8;
    return {static_cast<int>(word & 0x7f), ((word >> 15) & 1u) != 0};
}

class StripDecoder
{
public:
    // At most one event per frame. False = no change.
    bool decode(const std::uint8_t* frame, double timeMs, io::ControlEvent& out) noexcept;
    void reset() noexcept { contact_ = false; position_ = -1; }

private:
    bool contact_ = false;
    int position_ = -1;
};
} // namespace platform::imx6::strip
