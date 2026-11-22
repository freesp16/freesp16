// Pad velocity curves (spec 02-features/pads-and-scales.md PAD-002, 03-ui/control-surface.md UI-114): the reference's
// PAD CURVE list Hard 3, Hard 2, Hard 1, Linear (default), Soft 1, Soft 2, Soft 3, plus Sixteen's FIXED
// (`pads.velocityCurve`, `pads.fixedVelocity`). Applied on the message thread to hits of sources that do not shape
// velocity themselves (the hardware panel applies io::PadCurve in its reader thread, UI-118).
//
// Shape (our choice), the same law as io::PadCurve: x = raw / 127, Hard n = x^(1 + n/2), Soft n = x^(1 / (1 + n/2)),
// rounded, at least 1 for a hit; LINEAR is the identity; FIXED ignores the hit's strength. raw ≤ 0 = no hit (0).
#pragma once

#include <cstdint>

namespace ui::input::pads
{
enum class VelocityCurve : std::uint8_t { Hard3, Hard2, Hard1, Linear, Soft1, Soft2, Soft3, Fixed }; // pads.velocityCurve

int applyVelocityCurve(int raw, VelocityCurve, int fixed) noexcept;
} // namespace ui::input::pads
