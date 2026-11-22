// Drawing helpers shared by the visual renderers (screens/effects.md "Visual kinds"). The DrawList carries rounded
// rectangles and text only, so a curve is cached as rectangles: each segment of the polyline becomes 1–16 small stroke
// rectangles (about 3 lpx long and at least the stroke width wide, so steep and flat parts look equally thick) and each
// column a 12–16 % fill rectangle to the baseline. The cache is rebuilt only when the model changes; appending it is a
// copy.
#pragma once

#include "ui/render/DrawList.h"
#include "ui/screens/effects/VisualModel.h"
#include "ui/theme/Theme.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace ui::fxvis
{
namespace tk = ::theme::tokens;

class Polyline
{
public:
    explicit Polyline(std::size_t points = 257);
    // (xs[i], ys[i]), i < n; fillTo = baseline of the fill (NaN = no fill).
    void build(const float* xs, const float* ys, int n, float width, float fillTo);
    void emit(DrawList&, std::uint32_t stroke, std::uint32_t fill) const;
    std::size_t rects() const noexcept { return stroke_.size() + fill_.size(); }

private:
    std::vector<RectF> stroke_, fill_;
};

// The visual zone's inset box (bg, radius 6).
void zoneBox(DrawList&, const tk::Palette&, RectF zone);
// Log-frequency x of f (20 Hz … 20 kHz across x0…x1).
inline float logX(float f, float x0, float x1) noexcept
{
    return x0 + (std::log10(std::max(f, 1.0f)) - 1.30103f) / 3.0f * (x1 - x0);
}
// 100 / 1K / 10K grid lines with captions.
void freqGrid(DrawList&, const tk::Palette&, float x0, float x1, float y0, float y1);
// A 1 lpx line between two points (axis-aligned only).
inline void hline(DrawList& dl, float xa, float xb, float y, std::uint32_t c, float w = 1)
{
    dl.fill({xa, y - w / 2, xb - xa, w}, c);
}
inline void vline(DrawList& dl, float x, float ya, float yb, std::uint32_t c, float w = 1)
{
    dl.fill({x - w / 2, ya, w, yb - ya}, c);
}
// The slot ink with the model's opacity applied.
inline std::uint32_t ink(const VisualModel& m, const tk::Palette& p, float alpha = 1) noexcept
{
    return theme::withOpacity(m.colour != 0 ? m.colour : p.accent, alpha * m.opacity);
}
} // namespace ui::fxvis
