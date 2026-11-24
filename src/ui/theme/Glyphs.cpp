#include "ui/theme/Glyphs.h"

#include <algorithm>
#include <cmath>

namespace ui::glyph
{
namespace
{
constexpr float kDotPitch = 0.75f; // lpx between the dots of a diagonal stroke

struct Pen
{
    DrawList& dl;
    float x, y, k, w;
    std::uint32_t c;
    void l(float x1, float y1, float x2, float y2) const
    {
        line(dl, x + x1 * k, y + y1 * k, x + x2 * k, y + y2 * k, w, c);
    }
    void poly(std::initializer_list<float> xy, bool close = false) const
    {
        const float* p = xy.begin();
        const auto n = static_cast<int>(xy.size() / 2);
        for (int i = 0; i + 1 < n; ++i)
            l(p[2 * i], p[2 * i + 1], p[2 * i + 2], p[2 * i + 3]);
        if (close && n > 2)
            l(p[2 * n - 2], p[2 * n - 1], p[0], p[1]);
    }
    void ring(float cx, float cy, float r) const
    {
        dl.stroke({x + (cx - r) * k, y + (cy - r) * k, 2 * r * k, 2 * r * k}, c, w, r * k);
    }
    void dot(float cx, float cy, float r) const
    {
        dl.fill({x + (cx - r) * k, y + (cy - r) * k, 2 * r * k, 2 * r * k}, c, r * k);
    }
    void box(float rx, float ry, float rw, float rh, float radius = 1) const
    {
        dl.fill({x + rx * k, y + ry * k, rw * k, rh * k}, c, radius * k);
    }
    void arc(float cx, float cy, float r, float a0, float a1) const
    {
        const float len = std::abs(a1 - a0) * r * k;
        const int n = std::max(2, static_cast<int>(std::ceil(len / kDotPitch)));
        for (int i = 0; i <= n; ++i)
        {
            const float a = a0 + (a1 - a0) * static_cast<float>(i) / static_cast<float>(n);
            const float px = x + (cx + r * std::cos(a)) * k, py = y + (cy + r * std::sin(a)) * k;
            dl.fill({px - w * 0.5f, py - w * 0.5f, w, w}, c, w * 0.5f);
        }
    }
};
} // namespace

void line(DrawList& dl, float x1, float y1, float x2, float y2, float width, std::uint32_t argb)
{
    const float dx = x2 - x1, dy = y2 - y1, h = width * 0.5f;
    if (std::abs(dx) < 1e-3f || std::abs(dy) < 1e-3f) // axis-aligned: one rounded rect
    {
        dl.fill({std::min(x1, x2) - h, std::min(y1, y2) - h, std::abs(dx) + width, std::abs(dy) + width}, argb, h);
        return;
    }
    const float len = std::sqrt(dx * dx + dy * dy);
    const int n = std::max(1, static_cast<int>(std::ceil(len / kDotPitch)));
    for (int i = 0; i <= n; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(n);
        dl.fill({x1 + dx * t - h, y1 + dy * t - h, width, width}, argb, h);
    }
}

void draw(DrawList& dl, Kind kind, float x, float y, float size, std::uint32_t c)
{
    constexpr float pi = 3.14159265f;
    const float k = size / 24.0f;
    const Pen p{dl, x, y, k, std::max(1.5f, 2.0f * k), c};
    switch (kind)
    {
    case Kind::Close:
        p.l(7.5f, 7.5f, 16.5f, 16.5f);
        p.l(16.5f, 7.5f, 7.5f, 16.5f);
        break;
    case Kind::Check:
        p.poly({5, 12, 10, 17, 19, 7});
        break;
    case Kind::ChevronDown:
        p.poly({7, 10, 12, 15, 17, 10});
        break;
    case Kind::Backspace:
        p.poly({8, 6, 20, 6, 20, 18, 8, 18, 3, 12}, true);
        p.l(11, 9, 16, 15);
        p.l(16, 9, 11, 15);
        break;
    case Kind::Return:
        p.poly({19, 6, 19, 14, 6, 14});
        p.poly({10, 10, 6, 14, 10, 18});
        break;
    case Kind::Shift:
        p.poly({12, 4, 20, 12, 16, 12, 16, 19, 8, 19, 8, 12, 4, 12}, true);
        break;
    case Kind::ArrowLeft:
        p.l(5, 12, 19, 12);
        p.poly({10, 7, 5, 12, 10, 17});
        break;
    case Kind::ArrowRight:
        p.l(5, 12, 19, 12);
        p.poly({14, 7, 19, 12, 14, 17});
        break;
    case Kind::Lock: // 9 x 7 body under a 5 lpx shackle (mock.py strip(): rect + arc, 1.5 lpx), grid 12
    {
        const float s = size / 12.0f;
        dl.stroke({x + 2 * s, y, 5 * s, 7 * s}, c, 1.5f * s, 2.5f * s);
        dl.fill({x, y + 4.5f * s, 9 * s, 7 * s}, c, 1 * s);
        break;
    }
    case Kind::Info:
        p.ring(12, 12, 10);
        p.l(12, 11, 12, 17);
        p.dot(12, 7, 1.4f);
        break;
    case Kind::Error:
        p.ring(12, 12, 10);
        p.l(8, 8, 16, 16);
        p.l(16, 8, 8, 16);
        break;
    case Kind::Caution:
        p.poly({12, 3, 22, 21, 2, 21}, true);
        p.l(12, 9, 12, 14);
        p.dot(12, 17.5f, 1.3f);
        break;
    case Kind::Usb:
        p.poly({7, 3, 17, 3, 17, 12, 7, 12}, true);
        p.poly({5, 12, 19, 12, 19, 21, 5, 21}, true);
        break;
    case Kind::Project: // document with a folded corner and two text lines, 24 x 30 (mock.py frame())
        p.poly({0, 0, 16, 0, 24, 8, 24, 30, 0, 30}, true);
        p.poly({16, 0, 16, 8, 24, 8});
        p.l(4, 18, 20, 18);
        p.l(4, 24, 16, 24);
        break;
    case Kind::Back: // ↩ arrow, 32 x 28 (mock.py frame(back=True))
        p.l(0, 20, 22, 20);
        p.arc(22, 10, 10, 0.5f * pi, -0.5f * pi);
        p.l(22, 0, 8, 0);
        p.l(0, 20, 8, 12);
        p.l(0, 20, 8, 28);
        break;
    case Kind::Tracks:
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                p.box(2.0f + 6.0f * static_cast<float>(i), 2.0f + 6.0f * static_cast<float>(j), 4, 4);
        p.l(16, 22, 23, 15);
        break;
    case Kind::Mixer:
    {
        static constexpr float caps[] = {14, 6, 11};
        for (int i = 0; i < 3; ++i)
        {
            const float fx = 5.0f + 7.0f * static_cast<float>(i);
            p.l(fx, 2, fx, 22);
            p.box(fx - 3, caps[i], 6, 3);
        }
        break;
    }
    case Kind::Arranger:
        p.box(2, 3, 12, 4);
        p.box(6, 10, 14, 4);
        p.box(2, 17, 8, 4);
        break;
    case Kind::Score:
    case Kind::Gear:
        drawClassicToolbarGlyph(dl, kind, x, y, size, c);
        break;
    case Kind::Utility:
        p.ring(12, 12, 4);
        for (int i = 0; i < 8; ++i)
        {
            const float a = static_cast<float>(i) * pi / 4;
            p.l(12 + 7 * std::cos(a), 12 + 7 * std::sin(a), 12 + 10 * std::cos(a), 12 + 10 * std::sin(a));
        }
        break;
    }
}
} // namespace ui::glyph
