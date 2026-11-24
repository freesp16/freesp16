// The pad controller's lamps in the shell's lamp frame (03-ui/control-surface.md UI-150…152, 156).
// PadController::paintLamps resolves the 16 pad lamps of every pad mode, PLAY included, with one rule (io::padLamp:
// off / faint = silent / dim / on = selected / pulse = soloed / white flash = a hit by the user or the sequencer) and
// the four mode keys, at most 60 times a second. PadLamps is the LampSink it paints into: it keeps the last painted
// state exactly (io::LampMode::Level), so the composition root can lay it over every lamp frame of the shell's
// LampModel — a frame the throttle skips shows the previous paint (no flicker). Allocation-free.
#pragma once

#include "io/controllers/LampRules.h"
#include "ui/input/pads/PadController.h"

#include <array>

namespace ui::input::pads
{
class PadLamps final : public io::LampSink
{
public:
    // A resolved lamp back to a state: level 0 = off, ≤ kFaintLevel = faint, else that exact level (the colour is the
    // resolved one, so a flash keeps its fade); resolved at Normal brightness, the brightness preference applies later.
    void setLamp(io::Control, std::uint32_t rgb, std::uint8_t level) override;
    void flush() override { painted_ = true; }

    // The mode keys (UI-156: white while on, one-hot or none) and the 16 pads in every pad mode, PLAY too (UI-151/152:
    // the shell's PLAY-mode pads do not see the sequencer's hits). Nothing before the first paint.
    void overlay(io::LampFrame&) const noexcept;
    bool painted() const noexcept { return painted_; }
    const io::LampState& pad(int pad1to16) const noexcept { return pads_[static_cast<std::size_t>((pad1to16 - 1) & 15)]; }
    const io::LampState& modeKey(int i0to3) const noexcept { return keys_[static_cast<std::size_t>(i0to3 & 3)]; }

private:
    std::array<io::LampState, 16> pads_ {};
    std::array<io::LampState, 4> keys_ {};
    bool painted_ = false;
};
} // namespace ui::input::pads
