#include "ui/widgets/WaveformEditor.h"

#include "services/analysis/PeakKernels.h"
#include "ui/theme/Draw.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
constexpr float kPi = 3.14159265f;

void vline(DrawList& dl, float x, float y0, float y1, float w, std::uint32_t c)
{
    dl.fill({x - w / 2, y0, w, y1 - y0}, c);
}

const WaveOverview::Level* pickLevel(const WaveOverview& o, double framesPerColumn) noexcept
{
    const WaveOverview::Level* best = nullptr;
    for (const auto& l : o.levels) // the coarsest level whose bin is still ≤ one column
        if (l.bins > 0 && l.framesPerBin <= framesPerColumn * 1.01 &&
            (best == nullptr || l.framesPerBin > best->framesPerBin))
            best = &l;
    if (best == nullptr) // zoomed past the finest level: use the finest
        for (const auto& l : o.levels)
            if (l.bins > 0 && (best == nullptr || l.framesPerBin < best->framesPerBin))
                best = &l;
    return best;
}

// The overview as an exact min / max pyramid (WaveOverview::ch): level k + 1's bin = step[k] of level k's bins.
constexpr int kMaxLevels = 32;
struct Pyramid
{
    const WaveOverview* o = nullptr;
    int levels = 0;
    std::int64_t step[kMaxLevels]{};
};

// False without samples or without a usable finest level; a level that does not step up by a whole number (or whose
// bin count does not match) ends the pyramid there.
bool pyramidOf(const WaveOverview& o, Pyramid& p) noexcept
{
    if (o.ch == nullptr || o.channels <= 0 || o.frames <= 0 || o.levels.empty())
        return false;
    p.o = &o;
    p.levels = 0;
    const int n = static_cast<int>(std::min<std::size_t>(o.levels.size(), kMaxLevels));
    for (int k = 0; k < n; ++k)
    {
        const auto& l = o.levels[static_cast<std::size_t>(k)];
        const double f = l.framesPerBin;
        if (f < 1 || f != std::floor(f) || l.min == nullptr || l.max == nullptr)
            break;
        const auto fpb = static_cast<std::int64_t>(f);
        if (k == 0 && l.bins != (o.frames + fpb - 1) / fpb)
            break;
        if (k > 0)
        {
            const auto& prev = o.levels[static_cast<std::size_t>(k - 1)];
            const double r = f / prev.framesPerBin;
            const auto step = static_cast<std::int64_t>(r);
            if (r < 2 || r != std::floor(r) || l.bins != (prev.bins + step - 1) / step)
                break;
            p.step[k - 1] = step;
        }
        ++p.levels;
    }
    return p.levels > 0;
}

// The straight line between samples at frame t (clamped to the sample), every channel.
void traceAt(const WaveOverview& o, double t, float& lo, float& hi) noexcept
{
    t = std::clamp(t, 0.0, static_cast<double>(o.frames - 1));
    const auto i = static_cast<std::int64_t>(t), j = std::min(i + 1, o.frames - 1);
    const auto fr = static_cast<float>(t - static_cast<double>(i));
    for (int c = 0; c < o.channels; ++c)
    {
        const float v = o.ch[c][i] + (o.ch[c][j] - o.ch[c][i]) * fr;
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
}

void rawSpan(const WaveOverview& o, std::int64_t i0, std::int64_t i1, float& lo, float& hi) noexcept
{
    if (i1 > i0)
        for (int c = 0; c < o.channels; ++c)
            services::analysis::kernels::minMax(o.ch[c] + i0, static_cast<int>(i1 - i0), lo, hi);
}

// The exact min / max of the trace over frames [a, b]: its two ends, the samples between them — whole bins from the
// coarsest level that fits (at most step − 1 bins per side per level), raw samples at the < 1-bin ragged edges.
void exactSpan(const Pyramid& p, double a, double b, float& lo, float& hi) noexcept
{
    const auto& o = *p.o;
    traceAt(o, a, lo, hi);
    traceAt(o, b, lo, hi);
    const auto i0 = std::clamp<std::int64_t>(static_cast<std::int64_t>(std::ceil(a)), 0, o.frames);
    const auto i1 = std::clamp<std::int64_t>(static_cast<std::int64_t>(std::ceil(b)), i0, o.frames);
    const auto fpb = static_cast<std::int64_t>(o.levels[0].framesPerBin);
    std::int64_t b0 = (i0 + fpb - 1) / fpb, b1 = i1 / fpb; // the whole finest bins inside [i0, i1)
    if (b1 <= b0)
    {
        rawSpan(o, i0, i1, lo, hi);
        return;
    }
    rawSpan(o, i0, b0 * fpb, lo, hi);
    rawSpan(o, b1 * fpb, i1, lo, hi);
    auto take = [&](const WaveOverview::Level& l, std::int64_t from, std::int64_t to)
    {
        for (auto k = from; k < to; ++k)
        {
            lo = std::min(lo, l.min[k]);
            hi = std::max(hi, l.max[k]);
        }
    };
    for (int k = 0; b0 < b1; ++k)
    {
        const auto& l = o.levels[static_cast<std::size_t>(k)];
        if (k + 1 >= p.levels)
        {
            take(l, b0, b1);
            break;
        }
        const auto r = p.step[k];
        const auto u0 = std::min(b1, (b0 + r - 1) / r * r), u1 = std::max(u0, b1 / r * r);
        take(l, b0, u0);
        take(l, u1, b1);
        b0 = u0 / r;
        b1 = u1 / r;
    }
}
} // namespace

double WaveformEditor::windowLength(const WaveformModel& m) noexcept
{
    return static_cast<double>(m.frames) / std::max(1.0, m.zoomX);
}
double WaveformEditor::windowStart(const WaveformModel& m) noexcept
{
    const double len = windowLength(m), frames = static_cast<double>(m.frames);
    return std::clamp(m.zoomCentre * frames - len / 2, 0.0, std::max(0.0, frames - len));
}
float WaveformEditor::xOf(const WaveformModel& m, RectF plot, std::int64_t f) noexcept
{
    const double len = windowLength(m);
    if (len <= 0)
        return plot.x;
    return plot.x + static_cast<float>((static_cast<double>(f) - windowStart(m)) / len * static_cast<double>(plot.w));
}
std::int64_t WaveformEditor::frameAt(const WaveformModel& m, RectF plot, float x) noexcept
{
    const double f = windowStart(m) + static_cast<double>((x - plot.x) / plot.w) * windowLength(m);
    return static_cast<std::int64_t>(std::clamp(f, 0.0, static_cast<double>(m.frames)));
}

void WaveformEditor::drawColumns(DrawList& dl, const WaveOverview& o, double f0, double f1, int columns, float x,
                                 float pitch, float width, float cy, float h, std::int64_t in0, std::int64_t in1,
                                 std::uint32_t colour)
{
    if (columns <= 0 || f1 <= f0)
        return;
    const double per = (f1 - f0) / columns;
    Pyramid pyramid;
    const bool exact = pyramidOf(o, pyramid);
    const auto* level = exact ? nullptr : pickLevel(o, per);
    if (! exact && level == nullptr)
        return;
    const auto dim = theme::withOpacity(colour, 0.35f);
    const float half = width * 0.5f;
    for (int i = 0; i < columns; ++i)
    {
        const double a = f0 + per * i, b = a + per;
        float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
        if (exact)
            exactSpan(pyramid, a, b, lo, hi);
        else
        {
            auto b0 = static_cast<std::int64_t>(a / level->framesPerBin);
            auto b1 = std::max(b0 + 1, static_cast<std::int64_t>(std::ceil(b / level->framesPerBin - 1e-6)));
            b0 = std::clamp<std::int64_t>(b0, 0, level->bins - 1);
            b1 = std::clamp<std::int64_t>(b1, b0 + 1, level->bins);
            for (auto k = b0; k < b1; ++k)
            {
                lo = std::min(lo, level->min[k]);
                hi = std::max(hi, level->max[k]);
            }
        }
        const float xx = x + pitch * static_cast<float>(i) + pitch / 2;
        const float yTop = cy - hi * h, yBottom = cy - lo * h;
        const double mid = (a + b) / 2;
        const bool inside = mid >= static_cast<double>(in0) && mid <= static_cast<double>(in1);
        dl.fill({xx - half, yTop - half, width, std::max(0.0f, yBottom - yTop) + width}, inside ? colour : dim, half);
    }
}

void WaveformEditor::flag(DrawList& dl, const UiContext& ctx, float x, float y, std::string_view letter,
                          std::uint32_t fill, std::uint32_t ink, bool pointRight, bool outlineOnly)
{
    if (outlineOnly)
        dl.stroke({x, y, 24, 28}, fill, 1.5f, tk::radiusXs);
    else
    {
        dl.fill({x, y, 24, 28}, fill, tk::radiusXs);
        for (int k = 0; k < 8; ++k) // pointer: 6 wide, 16 tall, from the flag's side
        {
            const float w = 6.0f * static_cast<float>(8 - k) / 8.0f;
            const float px = pointRight ? x + 24 : x - w;
            dl.fill({px, y + 6 + static_cast<float>(k), w, 1}, fill);
            dl.fill({px, y + 21 - static_cast<float>(k), w, 1}, fill);
        }
    }
    dl.text(x + 12, y + 15, Align::Centre, TextStyleId::Label, ink, letter);
    (void)ctx;
}

void WaveformEditor::drawEditor(DrawList& dl, const UiContext& ctx, RectF r, const WaveformModel& m)
{
    const auto& p = ctx.palette;
    dl.fill(r, p.surface1, tk::radiusM);
    if (m.overview == nullptr || m.frames <= 0)
        return;
    const RectF plot{r.x + 8, r.y, r.w - 16, r.h};
    const auto col = m.colour;
    const float y0 = r.y + 4, y1 = r.bottom() - 4;
    const bool points = ! m.slicePoints.empty(); // slice starts in frames (moved / added points)
    if (points && m.slices == 0) // PLAYBACK: the grid at the slice points (SLC-011)
        for (std::size_t g = 1; g + 1 < m.slicePoints.size(); ++g)
        {
            const float gx = xOf(m, plot, m.slicePoints[g]);
            if (gx >= plot.x && gx <= plot.right())
                vline(dl, gx, r.y + 8, r.bottom() - 8, 1, theme::withOpacity(p.play, m.gridSnap ? 0.35f : 0.18f));
        }
    if (m.gridCount > 1 && m.slices == 0 && ! points)
        for (int g = 1; g < m.gridCount; ++g)
        {
            const auto f = static_cast<std::int64_t>(static_cast<double>(m.frames) * g / m.gridCount);
            const float gx = xOf(m, plot, f);
            if (gx < plot.x || gx > plot.right())
                continue;
            const bool bar = m.gridCount >= 2 && g % (m.gridCount / 2) == 0;
            vline(dl, gx, r.y + 8, r.bottom() - 8, 1,
                  theme::withOpacity(p.play, m.gridSnap ? (bar ? 0.7f : 0.35f) : 0.18f));
        }
    auto clampX = [&](float x) { return std::clamp(x, plot.x, plot.right()); };
    if (m.loop && m.slices == 0)
    {
        const float a = clampX(xOf(m, plot, m.loopStart)), b = clampX(xOf(m, plot, m.end));
        if (b > a)
            dl.fill({a, y0, b - a, r.h - 8}, theme::withOpacity(col, 0.12f));
    }
    if (points && m.slices > 0 && m.currentSlice > 0 && m.currentSlice < static_cast<int>(m.slicePoints.size()))
    {
        const float a = clampX(xOf(m, plot, m.slicePoints[static_cast<std::size_t>(m.currentSlice - 1)]));
        const float b = clampX(xOf(m, plot, m.slicePoints[static_cast<std::size_t>(m.currentSlice)]));
        if (b > a)
            dl.fill({a, y0, b - a, r.h - 8}, p.accentWeak);
    }
    else if (m.slices > 0 && m.currentSlice > 0)
    {
        const float sw = plot.w / static_cast<float>(m.slices);
        dl.fill({plot.x + sw * static_cast<float>(m.currentSlice - 1), y0, sw, r.h - 8}, p.accentWeak);
    }
    // Waveform band y+32 … h−32 (296 tall at 360), columns every 3 lpx, 1.8 wide (kit_track waveform()).
    const float bandTop = r.y + 32, bandH = r.h - 64, cy = bandTop + bandH / 2;
    const int columns = static_cast<int>(plot.w / 3);
    const double ws = windowStart(m), wl = windowLength(m);
    const std::int64_t in0 = m.slices > 0 ? 0 : m.start, in1 = m.slices > 0 ? m.frames : m.end;
    drawColumns(dl, *m.overview, ws, ws + wl, columns, plot.x, 3, 1.8f, cy,
                bandH / 2 * 0.92f * static_cast<float>(m.zoomY), in0, in1, col);
    draw::hline(dl, plot.x, plot.right(), cy, p.border);

    if (m.slices > 0 && points) // SET SLICE: lines at every slice start, boxes for the AREA's 16
    {
        const int n = std::min(m.slices, static_cast<int>(m.slicePoints.size()) - 1);
        for (int k = 0; k < n; ++k)
        {
            const float sx = xOf(m, plot, m.slicePoints[static_cast<std::size_t>(k)]);
            if (sx < plot.x - 1 || sx > plot.right() + 1)
                continue;
            const bool cur = k + 1 == m.currentSlice;
            vline(dl, sx, y0, y1, 1, theme::withOpacity(cur ? p.accent : col, 0.8f));
            if (k < m.firstBox || k >= m.firstBox + 16)
                continue;
            char num[12];
            std::snprintf(num, sizeof num, "%d", k + 1);
            const bool edited = k < 64 && ((m.slicesEdited >> k) & 1u) != 0;
            if (cur)
                dl.fill({sx + 2, r.y + 6, 24, 20}, p.accent, tk::radiusXs);
            else
            {
                dl.fill({sx + 2, r.y + 6, 24, 20}, p.surface1, tk::radiusXs);
                dl.stroke({sx + 2, r.y + 6, 24, 20}, col, edited ? 2.0f : 1.0f, tk::radiusXs);
            }
            if (cur && edited)
                dl.stroke({sx + 2, r.y + 6, 24, 20}, p.text1, 2, tk::radiusXs);
            dl.text(sx + 14, r.y + 17, Align::Centre, TextStyleId::Caption, cur ? p.onAccent : col, num);
            dl.fill({sx + 2, r.bottom() - 26, 24, 20}, cur ? p.accent : col, tk::radiusXs);
            dl.text(sx + 14, r.bottom() - 15, Align::Centre, TextStyleId::Caption, p.onAccent, num);
        }
        if (m.play >= 0)
        {
            const float px = xOf(m, plot, m.play);
            if (px >= plot.x && px <= plot.right())
                vline(dl, px, y0, y1, 2, p.playhead);
        }
        return;
    }
    if (m.slices > 0)
    {
        const float sw = plot.w / static_cast<float>(m.slices);
        for (int k = 0; k < m.slices; ++k)
        {
            const float sx = plot.x + sw * static_cast<float>(k);
            const bool cur = k + 1 == m.currentSlice;
            vline(dl, sx, y0, y1, 1, theme::withOpacity(cur ? p.accent : col, 0.8f));
            char num[12];
            std::snprintf(num, sizeof num, "%d", k + 1);
            if (cur)
                dl.fill({sx + 2, r.y + 6, 24, 20}, p.accent, tk::radiusXs);
            else
            {
                dl.fill({sx + 2, r.y + 6, 24, 20}, p.surface1, tk::radiusXs);
                dl.stroke({sx + 2, r.y + 6, 24, 20}, col, 1, tk::radiusXs);
            }
            dl.text(sx + 14, r.y + 17, Align::Centre, TextStyleId::Caption, cur ? p.onAccent : col, num);
            dl.fill({sx + 2, r.bottom() - 26, 24, 20}, cur ? p.accent : col, tk::radiusXs);
            dl.text(sx + 14, r.bottom() - 15, Align::Centre, TextStyleId::Caption, p.onAccent, num);
        }
        return;
    }
    const float xs = xOf(m, plot, m.start), xe = xOf(m, plot, m.end), xl = xOf(m, plot, m.loopStart);
    auto inPlot = [&](float x) { return x >= plot.x - 1 && x <= plot.right() + 1; };
    if (inPlot(xs))
        vline(dl, xs, y0, y1, 1, p.text1);
    if (inPlot(xe))
        vline(dl, xe, y0, y1, 1, p.text1);
    if (inPlot(xs))
        flag(dl, ctx, xs, y0, m.reverse ? "S\xE2\x86\xA4" : "S", p.surface3, p.text1, true);
    if (inPlot(xe))
        flag(dl, ctx, xe - 24, r.bottom() - 32, "E", p.surface3, p.text1, false);
    if (m.loop && inPlot(xl))
    {
        vline(dl, xl, y0, y1, 1, p.accent);
        flag(dl, ctx, xl, r.bottom() - 32, "L", p.accent, p.onAccent, true);
    }
    // Step-hold ghosts: the locked positions as mod-outlined flags beside the base flags (playback.md "Step held").
    static constexpr std::string_view letters[] = {"S", "L", "E"};
    for (int k = 0; k < 3; ++k)
        if (m.ghostMask & (1u << k))
        {
            const float gx = xOf(m, plot, m.lockGhosts[static_cast<std::size_t>(k)]);
            if (! inPlot(gx))
                continue;
            vline(dl, gx, y0, y1, 1, theme::withOpacity(p.mod, 0.8f));
            flag(dl, ctx, k == 2 ? gx - 24 : gx, k == 0 ? y0 : r.bottom() - 32, letters[k], p.mod, p.mod, k != 2, true);
        }
    if (m.play >= 0)
    {
        const float px = xOf(m, plot, m.play);
        if (inPlot(px))
            vline(dl, px, y0, y1, 2, p.playhead);
    }
    float ax = -1, ay = 0;
    if (m.activeCursor == 0 && inPlot(xs))
        ax = xs, ay = y0;
    else if (m.activeCursor == 1 && m.loop && inPlot(xl))
        ax = xl, ay = r.bottom() - 32;
    else if (m.activeCursor == 2 && inPlot(xe))
        ax = xe - 24, ay = r.bottom() - 32;
    if (ax >= 0)
        dl.stroke({ax - 4, ay - 4, 32, 36}, p.accent, tk::borderFocus, tk::radiusS);
}

void WaveformEditor::drawOverview(DrawList& dl, const UiContext& ctx, RectF r, const WaveformModel& m)
{
    const auto& p = ctx.palette;
    dl.fill(r, p.surface1, tk::radiusS);
    dl.text(r.x + 12, r.y + r.h / 2, Align::Centre, TextStyleId::Title, p.text3, "\xE2\x80\xB9");
    dl.text(r.right() - 12, r.y + r.h / 2, Align::Centre, TextStyleId::Title, p.text3, "\xE2\x80\xBA");
    if (m.overview == nullptr || m.frames <= 0)
        return;
    const float ix = r.x + 24, iw = r.w - 48;
    const auto frames = static_cast<double>(m.frames);
    if (m.loop)
    {
        const float a = ix + iw * static_cast<float>(static_cast<double>(m.loopStart) / frames);
        const float b = ix + iw * static_cast<float>(static_cast<double>(m.end) / frames);
        if (b > a)
            dl.fill({a, r.y + 4, b - a, r.h - 8}, theme::withOpacity(m.colour, 0.12f));
    }
    const int columns = static_cast<int>(iw / 2);
    drawColumns(dl, *m.overview, 0, frames, columns, ix, 2, 1.2f, r.y + r.h / 2, (r.h - 12) / 2 * 0.85f, 0, m.frames,
                m.colour);
    draw::hline(dl, ix, ix + iw, r.y + r.h / 2, p.border);
    if (m.slices > 0 && ! m.slicePoints.empty()) // a tick per slice start at its frame
        for (int k = 0; k < m.slices && k + 1 < static_cast<int>(m.slicePoints.size()); ++k)
            dl.fill({ix + iw * static_cast<float>(static_cast<double>(m.slicePoints[static_cast<std::size_t>(k)]) / frames) - 1,
                     r.bottom() - 8, 3, 6},
                    p.text1);
    else if (m.slices > 0)
        for (int k = 0; k < m.slices; ++k)
            dl.fill({ix + iw * static_cast<float>(k) / static_cast<float>(m.slices) - 1, r.bottom() - 8, 3, 6},
                    p.text1);
    const double ws = windowStart(m) / frames, we = (windowStart(m) + windowLength(m)) / frames;
    const float a = ix + iw * static_cast<float>(ws), b = ix + iw * static_cast<float>(we);
    dl.stroke({a, r.y + 2, b - a, r.h - 4}, p.text1, 2, tk::radiusXs);
}

void WaveformEditor::drawZoomGauge(DrawList& dl, const UiContext& ctx, float x, float y, float level)
{
    const auto& p = ctx.palette;
    auto arc = [&](float phi0, float phi1, std::uint32_t c) // φ from the bottom (0) to the top (π), left half-ellipse
    {
        const int n = std::max(1, static_cast<int>((phi1 - phi0) * 56 / 2));
        for (int i = 0; i <= n; ++i)
        {
            const float phi = phi0 + (phi1 - phi0) * static_cast<float>(i) / static_cast<float>(n);
            dl.fill({x + 48 - 48 * std::sin(phi) - 2, y + 64 + 64 * std::cos(phi) - 2, 4, 4}, c, 2);
        }
    };
    arc(0, kPi, p.borderStrong);
    if (level > 0.005f)
        arc(0, kPi * std::min(1.0f, level), p.accent);
    dl.text(x + 30, y + 26, Align::Centre, TextStyleId::Title, p.text2, "+");
    dl.text(x + 26, y + 64, Align::Centre, TextStyleId::Body, p.text2, "\xE2\x86\x94");
    dl.text(x + 30, y + 102, Align::Centre, TextStyleId::Title, p.text2, "\xE2\x88\x92");
}
} // namespace ui
