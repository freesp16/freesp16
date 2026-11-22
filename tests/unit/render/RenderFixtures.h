// Shared fixtures of the render tests and the render benchmark: the reference DrawList (every op, radius, stroke,
// alpha and text style, fractional edges) and image comparison helpers. Header-only; no ui/screens dependency.
#pragma once

#include "ui/render/DrawList.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace rendertest
{
// The reference frame of the renderer contract: background, rounded fills and outlines of
// every radius class, translucent overlays, hairlines on fractional positions, every text style and alignment, the
// UI's symbols, and shapes clipped by the canvas edge.
inline void referenceList(ui::DrawList& dl, int variant = 0)
{
    using ui::Align;
    using ui::TextStyleId;
    dl.fill({0, 0, 1280, 768}, 0xFF0B0D10u);
    dl.fill({0, 40, 1280, 96}, 0xFF13161Bu);
    dl.fill({0, 136, 1280, 1}, 0xFF262B33u);
    for (int i = 0; i < 8; ++i)
    {
        const float x = 16.0f + 156.5f * i, r = static_cast<float>(i * 3);
        dl.fill({x, 160.25f, 140, 90}, 0xFF1A1E25u, r);
        dl.stroke({x + 1, 161.25f, 138, 88}, 0xFFFF7E72u + static_cast<std::uint32_t>(i * 0x1020), 1.0f + 0.5f * i, r);
        dl.fill({x + 10, 200.6f, 60.3f + variant, 12.7f}, 0x80FFFFFFu, 2);
    }
    const TextStyleId styles[] = {TextStyleId::Caption,    TextStyleId::Label,  TextStyleId::Body,
                                  TextStyleId::BodyStrong, TextStyleId::Title,  TextStyleId::ValueS,
                                  TextStyleId::Value,      TextStyleId::ValueL, TextStyleId::Display};
    float y = 290;
    for (int i = 0; i < 9; ++i)
    {
        const auto s = styles[i];
        dl.text(24, y, Align::Left, s, 0xFFE8ECF2u,
                i < 5 ? "TRACKS \xE2\x80\xBA Kick 909 \xC3\xA9\xC3\xB1" : "-12.5 dB 0123456789");
        dl.text(1256, y, Align::Right, s, 0xCC9AA3B0u,
                "\xE2\x80\x9CQ\xE2\x80\x9D \xE2\x9C\x95 \xE2\x9C\x93 \xE2\x96\xB6");
        y += i < 6 ? 34.0f : 52.0f;
    }
    dl.text(640, 740, Align::Centre, TextStyleId::Label, 0xFFFFD166u,
            "\xE2\x80\xB9 SWING 54 % \xE2\x80\xBA \xE2\x9F\xB2 \xE2\x88\x9E");
    dl.fill({1176, 184, 8, 416}, 0xFF262B33u, 2);
    dl.fill({1176, 300.4f + 3.0f * variant, 8, 299.6f - 3.0f * variant}, 0xFF3DDC84u, 2);
    dl.fill({1200, 400, 120, 80}, 0x6000A0FFu, 12); // crosses the canvas edge
    dl.stroke({-10, 700, 60, 40}, 0xFFFFFFFFu, 2, 8);
}

struct Compare
{
    double match = 0; // share of pixels within tolerance after the blur
    int maxDiff = 0;  // largest raw per-channel difference
    long exactDiffs = 0;
};

// The snapshot metric: both images blurred with a 5 x 5 box filter, a pixel matches when its
// maximum channel difference is <= tolerance.
inline Compare compare(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b, int w, int h,
                       int tolerance = 24)
{
    Compare c;
    auto ch = [&](const std::vector<std::uint32_t>& img, int x, int y, int shift)
    { return static_cast<int>((img[static_cast<std::size_t>(y * w + x)] >> shift) & 255u); };
    long within = 0, total = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            int worst = 0;
            for (int shift : {16, 8, 0})
            {
                const int raw = std::abs(ch(a, x, y, shift) - ch(b, x, y, shift));
                c.maxDiff = std::max(c.maxDiff, raw);
                int sa = 0, sb = 0;
                for (int dy = -2; dy <= 2; ++dy)
                    for (int dx = -2; dx <= 2; ++dx)
                    {
                        const int xx = std::clamp(x + dx, 0, w - 1), yy = std::clamp(y + dy, 0, h - 1);
                        sa += ch(a, xx, yy, shift);
                        sb += ch(b, xx, yy, shift);
                    }
                worst = std::max(worst, std::abs(sa - sb) / 25);
            }
            c.exactDiffs += (a[static_cast<std::size_t>(y * w + x)] & 0xFFFFFFu) !=
                            (b[static_cast<std::size_t>(y * w + x)] & 0xFFFFFFu);
            within += worst <= tolerance;
            ++total;
        }
    c.match = static_cast<double>(within) / static_cast<double>(total);
    return c;
}

inline std::vector<std::uint32_t> rgbOnly(std::vector<std::uint32_t> px)
{
    for (auto& p : px)
        p &= 0x00FFFFFFu;
    return px;
}
} // namespace rendertest
