// The pad controller's lamps in the shell's lamp frame (03-ui/control-surface.md UI-150…152, 156).
// PadController::paintLamps resolves the pad lamps of the current pad mode (MUTE: muted tracks dimmed; TRACK: the
// selected track brighter; SLICE: the selected track's slice pads; SCALE: root notes lit, the others dim; hit flashes)
// and the four mode keys, at most 60 times a second. PadLamps is the LampSink it paints into: it keeps the last painted
// state, so the composition root can lay it over every lamp frame of the shell's LampModel — a frame the throttle
// skips shows the previous paint, never the shell's pad-mode-less pads (no flicker). Allocation-free.
#pragma once

#include "io/controllers/LampRules.h"
#include "ui/input/pads/PadController.h"

#include <array>

namespace ui::input::pads
{
class PadLamps final : public io::LampSink
{
public:
    // A resolved lamp back to a state: level 0 = off, >= 200 = on (the colour is the resolved one, so a flash keeps
    // its fade), else dim; the same levels resolve() gives (Normal brightness), the brightness preference applies later.
    void setLamp(io::Control, std::uint32_t rgb, std::uint8_t level) override;
    void flush() override { painted_ = true; }

    // The mode keys always (UI-156: white while on, one-hot or none); the pads while a pad mode is on (UI-152) — in
    // PLAY mode the shell's pad lamps stay (UI-151: track colour by content, hit flash). Nothing before the first paint.
    void overlay(io::LampFrame&, PadMode) const noexcept;
    bool painted() const noexcept { return painted_; }
    const io::LampState& pad(int pad1to16) const noexcept { return pads_[static_cast<std::size_t>((pad1to16 - 1) & 15)]; }
    const io::LampState& modeKey(int i0to3) const noexcept { return keys_[static_cast<std::size_t>(i0to3 & 3)]; }

private:
    std::array<io::LampState, 16> pads_ {};
    std::array<io::LampState, 4> keys_ {};
    bool painted_ = false;
};
} // namespace ui::input::pads
