// The panel's pad preferences hand-over and their mapping onto io::PadCurve (see PanelPads.h).
#include "io/controllers/PanelPads.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace io
{
namespace
{
struct Shared
{
    std::atomic<std::uint32_t> generation {0};
    std::atomic<int> curve {3}, fixedVelocity {100}, threshold {5}, sensitivity {5};
};
Shared& shared() noexcept
{
    static Shared s;
    return s;
}
} // namespace

void applyPanelPadPrefs(const PanelPadPrefs& p, const PadCalibration& base, PadCalibration& c, PadResponse& r) noexcept
{
    c = base;
    r = {};
    const int curve = std::clamp(p.curve, 0, 7);
    r.shape = curve == 7 ? PadCurveShape::Fixed : static_cast<PadCurveShape>(curve - 3);
    r.fixedVelocity = std::clamp(p.fixedVelocity, 1, 127);
    const int t = std::clamp(p.threshold, 1, 16), s = std::clamp(p.sensitivity, 1, 16);
    c.threshold = static_cast<std::uint16_t>(std::clamp(base.threshold + (t - 5) * 8, 1, PadCurve::kWordMax));
    const double gain = std::pow(2.0, (s - 5) / (s < 5 ? 4.0 : 11.0));
    const double span = std::max(1, static_cast<int>(base.ceiling) - static_cast<int>(base.floor));
    const int ceiling = base.floor + std::max(1, static_cast<int>(std::lround(span / gain)));
    c.ceiling = static_cast<std::uint16_t>(std::min(ceiling, PadCurve::kWordMax));
}

void publishPanelPads(const PanelPadPrefs& p) noexcept
{
    auto& s = shared();
    s.curve.store(p.curve, std::memory_order_relaxed);
    s.fixedVelocity.store(p.fixedVelocity, std::memory_order_relaxed);
    s.threshold.store(p.threshold, std::memory_order_relaxed);
    s.sensitivity.store(p.sensitivity, std::memory_order_relaxed);
    s.generation.fetch_add(1, std::memory_order_release);
}

bool takePanelPads(std::uint32_t& seen, PanelPadPrefs& out) noexcept
{
    auto& s = shared();
    const auto g = s.generation.load(std::memory_order_acquire);
    if (g == seen)
        return false;
    out.curve = s.curve.load(std::memory_order_relaxed);
    out.fixedVelocity = s.fixedVelocity.load(std::memory_order_relaxed);
    out.threshold = s.threshold.load(std::memory_order_relaxed);
    out.sensitivity = s.sensitivity.load(std::memory_order_relaxed);
    seen = g;
    return true;
}
} // namespace io
