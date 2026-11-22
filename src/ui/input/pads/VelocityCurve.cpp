#include "ui/input/pads/VelocityCurve.h"

#include <cmath>

namespace ui::input::pads
{
int applyVelocityCurve(int raw, VelocityCurve curve, int fixed) noexcept
{
    if (raw <= 0)
        return 0;
    if (curve == VelocityCurve::Fixed)
        return fixed < 1 ? 1 : (fixed > 127 ? 127 : fixed);
    const int v = raw > 127 ? 127 : raw;
    const int n = static_cast<int>(curve) - static_cast<int>(VelocityCurve::Linear); // −3 … +3
    if (n == 0)
        return v;
    const double k = 1.0 + 0.5 * std::abs(n);
    const double exponent = n < 0 ? k : 1.0 / k; // Hard: steeper (softer hits quieter), Soft: flatter
    const long out = std::lround(127.0 * std::pow(static_cast<double>(v) / 127.0, exponent));
    return out < 1 ? 1 : (out > 127 ? 127 : static_cast<int>(out));
}
} // namespace ui::input::pads
