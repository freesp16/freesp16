#include "ui/screens/effects/visuals/VisualDraw.h"

#include <algorithm>

namespace ui::fxvis
{
Polyline::Polyline(std::size_t points)
{
    stroke_.reserve(points * 16); // the worst case: every segment split 16 times
    fill_.reserve(points);
}

void Polyline::build(const float* xs, const float* ys, int n, float width, float fillTo)
{
    stroke_.clear();
    fill_.clear();
    const float h = width / 2;
    for (int i = 0; i + 1 < n; ++i)
    {
        const float xa = xs[i], ya = ys[i], xb = xs[i + 1], yb = ys[i + 1];
        const float dx = xb - xa, dy = yb - ya;
        // Sub-rectangles about 3 lpx long: short enough that slopes read as lines, not stairs.
        const int m = std::clamp(static_cast<int>(std::ceil(std::max(std::abs(dx), std::abs(dy)) / 3.0f)), 1, 16);
        for (int j = 0; j < m; ++j)
        {
            const float t0 = static_cast<float>(j) / static_cast<float>(m), t1 = static_cast<float>(j + 1) / m;
            const float cx = xa + dx * (t0 + t1) / 2, y0 = ya + dy * t0, y1 = ya + dy * t1;
            const float w = std::max(std::abs(dx) / static_cast<float>(m), width);
            stroke_.push_back({cx - w / 2, std::min(y0, y1) - h, w, std::abs(y1 - y0) + width});
        }
        if (! std::isnan(fillTo))
        {
            const float y = (ya + yb) / 2;
            fill_.push_back({xa, std::min(y, fillTo), dx, std::abs(fillTo - y)});
        }
    }
}

void Polyline::emit(DrawList& dl, std::uint32_t stroke, std::uint32_t fill) const
{
    for (const auto& r : fill_)
        dl.fill(r, fill);
    for (const auto& r : stroke_)
        dl.fill(r, stroke, 1);
}

void zoneBox(DrawList& dl, const tk::Palette& p, RectF zone)
{
    dl.fill(zone, p.bg, 6);
}

void freqGrid(DrawList& dl, const tk::Palette& p, float x0, float x1, float y0, float y1)
{
    constexpr struct
    {
        float f;
        const char* label;
    } lines[] = {{100, "100"}, {1000, "1K"}, {10000, "10K"}};
    for (const auto& l : lines)
    {
        const float x = logX(l.f, x0, x1);
        vline(dl, x, y0 + 8, y1 - 8, p.border);
        dl.text(x + 4, y1 - 14, Align::Left, TextStyleId::Caption, p.text3, l.label);
    }
}
} // namespace ui::fxvis
