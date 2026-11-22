#include "ui/widgets/EnvelopeEditor.h"

#include "ui/theme/Draw.h"
#include "ui/theme/Glyphs.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
constexpr int kMaxPoints = 96;

struct Pt
{
    float x, y;
};
struct Poly
{
    std::array<Pt, kMaxPoints> p{};
    int n = 0;
    void add(float x, float y) noexcept
    {
        if (n < kMaxPoints)
            p[static_cast<std::size_t>(n++)] = {x, y};
    }
};

float holdEnd(const EnvelopeModel& m) noexcept
{
    return std::max(m.attackS, m.holdS);
}

// The shape as a polyline in plot coordinates (x monotonic) inside `plot`, for the model's window.
Poly shape(const EnvelopeModel& m, RectF plot)
{
    Poly poly;
    const float w = std::max(1e-4f, m.windowS);
    auto X = [&](float s) { return plot.x + plot.w * std::min(1.0f, s / w); };
    const float top = plot.y, bottom = plot.bottom(), right = plot.right();
    const float xa = X(m.attackS);
    poly.add(plot.x, bottom);
    if (m.a == EnvCurve::Curve && xa - plot.x > 4) // bowed attack: fast rise (quadratic, control near the top-left)
        for (int i = 1; i <= 16; ++i)
        {
            const float t = static_cast<float>(i) / 16;
            const float cx = plot.x + (xa - plot.x) * 0.1f, cy = top + (bottom - top) * 0.05f;
            poly.add((1 - t) * (1 - t) * plot.x + 2 * t * (1 - t) * cx + t * t * xa,
                     (1 - t) * (1 - t) * bottom + 2 * t * (1 - t) * cy + t * t * top);
        }
    else
        poly.add(xa, top);
    if (m.holdInf || holdEnd(m) >= w)
    {
        poly.add(right, top);
        return poly;
    }
    const float xh = X(holdEnd(m));
    poly.add(xh, top);
    if (m.releaseInf)
    {
        poly.add(right, top);
        return poly;
    }
    const float xr = X(holdEnd(m) + m.releaseS);
    if (m.r == EnvCurve::Curve && xr - xh > 4) // kit mock: Q(xh + 60, bottom − 20) at a 585 lpx release
    {
        const float cx = xh + (xr - xh) * 0.103f, cy = bottom - (bottom - top) * 0.053f;
        for (int i = 1; i <= 40; ++i)
        {
            const float t = static_cast<float>(i) / 40;
            poly.add((1 - t) * (1 - t) * xh + 2 * t * (1 - t) * cx + t * t * xr,
                     (1 - t) * (1 - t) * top + 2 * t * (1 - t) * cy + t * t * bottom);
        }
    }
    else
        poly.add(xr, bottom);
    if (xr < right)
        poly.add(right, bottom);
    return poly;
}

void stroke(DrawList& dl, const Poly& poly, float width, std::uint32_t c)
{
    for (int i = 1; i < poly.n; ++i)
    {
        const auto a = poly.p[static_cast<std::size_t>(i - 1)], b = poly.p[static_cast<std::size_t>(i)];
        glyph::line(dl, a.x, a.y, b.x, b.y, width, c);
    }
}

void dashed(DrawList& dl, const Poly& poly, float width, std::uint32_t c)
{
    constexpr float on = 6, off = 5;
    float phase = 0;
    for (int i = 1; i < poly.n; ++i)
    {
        const auto a = poly.p[static_cast<std::size_t>(i - 1)], b = poly.p[static_cast<std::size_t>(i)];
        const float len = std::hypot(b.x - a.x, b.y - a.y);
        float s = 0;
        while (s < len)
        {
            const float cycle = std::fmod(phase + s, on + off);
            const float step = cycle < on ? std::min(on - cycle, len - s) : std::min(on + off - cycle, len - s);
            if (cycle < on)
            {
                const float t0 = s / len, t1 = (s + step) / len;
                glyph::line(dl, a.x + (b.x - a.x) * t0, a.y + (b.y - a.y) * t0, a.x + (b.x - a.x) * t1,
                            a.y + (b.y - a.y) * t1, width, c);
            }
            s += std::max(step, 0.5f);
        }
        phase += len;
    }
}

float yAt(const Poly& poly, float x) noexcept
{
    for (int i = 1; i < poly.n; ++i)
    {
        const auto a = poly.p[static_cast<std::size_t>(i - 1)], b = poly.p[static_cast<std::size_t>(i)];
        if (x <= b.x)
            return b.x - a.x < 1e-3f ? b.y : a.y + (b.y - a.y) * (x - a.x) / (b.x - a.x);
    }
    return poly.p[static_cast<std::size_t>(poly.n - 1)].y;
}

void handle(DrawList& dl, const UiContext& ctx, RectF r, std::string_view letter, bool inf, bool focused)
{
    const auto& p = ctx.palette;
    dl.fill(r, p.surface3, tk::radiusXs);
    dl.text(r.x + 14, r.y + (inf ? 11 : 15), Align::Centre, TextStyleId::Label, p.text1, letter);
    if (inf)
        dl.text(r.x + 14, r.y + 22, Align::Centre, TextStyleId::Caption, p.text2, "\xE2\x88\x9E");
    if (focused)
        dl.stroke({r.x - 4, r.y - 4, 36, 36}, p.accent, tk::borderFocus, tk::radiusS);
}
} // namespace

float EnvelopeEditor::xOf(const EnvelopeModel& m, float s) noexcept
{
    return kPlot.x + kPlot.w * std::min(1.0f, s / std::max(1e-4f, m.windowS));
}

float EnvelopeEditor::fitWindow(const EnvelopeModel& m) noexcept
{
    float target;
    if (m.holdInf)
        target = std::max(m.attackS / 0.3f, 0.1f) * 1.0f;
    else if (m.releaseInf)
        target = holdEnd(m) / 0.55f;
    else
        target = (holdEnd(m) + m.releaseS) / 0.85f;
    target = std::max(target, 0.1f);
    if (m.holdInf && m.attackS < 0.03f)
        target = 1.0f; // the default rectangle: a 1 s window
    if (target < 1.0f)
        return std::ceil(target / 0.1f - 1e-3f) * 0.1f;
    if (target < 10.0f)
        return std::ceil(target - 1e-3f);
    return std::ceil(target / 5.0f - 1e-3f) * 5.0f;
}

RectF EnvelopeEditor::handleRect(const EnvelopeModel& m, int h) noexcept
{
    const float right = kPlot.right() - 56; // INFINITY handles sit at x 1176
    switch (h)
    {
    case 0:
        return {xOf(m, m.attackS) + 2, kPlot.y + 4, 28, 28};
    case 1:
        return {(m.holdInf ? right : xOf(m, holdEnd(m))) - 14, kPlot.y + 4, 28, 28};
    default:
        return {(m.releaseInf || m.holdInf ? right : xOf(m, holdEnd(m) + m.releaseS)) - 14, kPlot.bottom() - 32, 28,
                28};
    }
}

void EnvelopeEditor::draw(DrawList& dl, const UiContext& ctx, const EnvelopeModel& m)
{
    const auto& p = ctx.palette;
    dl.fill(kPanel, p.surface1, tk::radiusM);
    const auto plot = kPlot;
    for (int k = 0; k < 6; ++k)
    {
        const float xx = plot.x + plot.w * static_cast<float>(k) / 5;
        draw::vline(dl, xx, plot.y, plot.bottom(), p.border);
        char lab[24];
        const float s = m.windowS * static_cast<float>(k) / 5;
        if (k == 0)
            std::snprintf(lab, sizeof lab, "0");
        else if (m.sync) // UID-728: note values — sixteenths of a bar reduced (1/16, 1/4 …), then bars
        {
            const double steps = static_cast<double>(s) * static_cast<double>(m.bpm) / 15.0;
            if (steps >= 15.5)
            {
                const double bars = std::round(steps / 16.0 * 10.0) / 10.0;
                std::snprintf(lab, sizeof lab, "%g BAR%s", std::min(bars, 999.0), bars > 1.0 ? "S" : "");
            }
            else
            {
                int n = std::max(1, static_cast<int>(std::lround(steps))), d = 16;
                while (n % 2 == 0 && d > 1)
                    n /= 2, d /= 2;
                std::snprintf(lab, sizeof lab, "%d/%d", n, d);
            }
        }
        else if (m.windowS < 2.0f)
            std::snprintf(lab, sizeof lab, "%d ms", static_cast<int>(std::lround(s * 1000)));
        else
            std::snprintf(lab, sizeof lab, "%g s", static_cast<double>(std::round(s * 10) / 10));
        dl.text(xx, plot.bottom() + 24, Align::Centre, TextStyleId::Caption, p.text3, lab);
    }
    draw::hline(dl, plot.x, plot.right(), plot.bottom(), p.borderStrong);
    char vel[8];
    std::snprintf(vel, sizeof vel, "%d", m.velocity);
    dl.text(plot.x - 8, plot.y + 4, Align::Right, TextStyleId::Caption, p.text3, vel);

    const auto poly = shape(m, plot);
    const auto fill = theme::withOpacity(m.colour, 0.12f);
    const float xEnd = poly.p[static_cast<std::size_t>(poly.n - 1)].x;
    for (float x = plot.x; x < xEnd; x += 2) // 12 % fill under the curve, 2 lpx columns
    {
        const float y = yAt(poly, x + 1);
        if (y < plot.bottom() - 0.5f)
            dl.fill({x, y, 2, plot.bottom() - y}, fill);
    }
    stroke(dl, poly, 2, m.colour);
    if (m.gate && ! m.holdInf) // GATE: dashed "pad release" marker at the end of hold
    {
        Poly marker;
        const float xh = xOf(m, holdEnd(m));
        marker.add(xh, plot.y);
        marker.add(xh, plot.bottom());
        dashed(dl, marker, 1, p.text3);
    }
    if (m.lockGhost != nullptr)
    {
        auto ghost = *m.lockGhost;
        ghost.windowS = m.windowS;
        dashed(dl, shape(ghost, plot), 1.5f, p.mod);
    }
    if (m.playheadS >= 0)
    {
        const float x = xOf(m, m.playheadS);
        const float y = yAt(poly, x);
        dl.fill({x - 7, y - 7, 14, 14}, p.playhead, 7);
    }
    handle(dl, ctx, handleRect(m, 0), "A", false, m.activeHandle == 0);
    handle(dl, ctx, handleRect(m, 1), "H", m.holdInf, m.activeHandle == 1);
    handle(dl, ctx, handleRect(m, 2), "R", m.releaseInf, m.activeHandle == 2);
}

void EnvelopeEditor::drawThumb(DrawList& dl, const UiContext&, RectF r, const EnvelopeModel& m)
{
    // kit_track env_thumb(): attack / hold / release as fractions of the thumbnail (hold INF = 55 %).
    const float x0 = r.x + 14, y0 = r.y + 12, y1 = r.bottom() - 10, w = r.w - 28;
    const float total = std::max(1e-3f, m.windowS);
    const float a = std::min(1.0f, m.attackS / total), hd = m.holdInf ? 1.0f : std::min(1.0f, holdEnd(m) / total),
                rl = m.releaseInf ? 1.0f : std::min(1.0f, m.releaseS / total);
    const float xa = x0 + a * w * 0.3f;
    const float xh = x0 + w * (hd >= 1 ? 0.55f : 0.3f + hd * 0.3f);
    const float xr = xh + w * 0.45f * rl;
    Poly poly;
    poly.add(x0, y1);
    poly.add(xa, y0);
    poly.add(xh, y0);
    poly.add(xr, y1);
    const auto fill = theme::withOpacity(m.colour, 0.12f);
    for (float x = x0; x < xr; x += 2)
    {
        const float y = yAt(poly, x + 1);
        dl.fill({x, y, 2, y1 - y}, fill);
    }
    stroke(dl, poly, 2, m.colour);
}
} // namespace ui
