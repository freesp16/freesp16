#include "ui/widgets/DataTable.h"

#include <algorithm>
#include <cmath>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
}

void DataTable::drawFrame(DrawList& dl, const UiContext& ctx, const DataTableModel& m)
{
    const auto& p = ctx.palette;
    const auto r = m.panel;
    dl.fill(r, p.surface1, tk::radiusM);
    dl.fill({r.x, r.y, r.w, m.headerH}, p.surface3, tk::radiusM);
    dl.fill({r.x, r.y + m.headerH - 8, r.w, 8}, p.surface3);
    for (const auto& c : m.columns)
    {
        const float cx = r.x + c.x + c.w / 2;
        if (c.sub.empty())
            dl.text(cx, r.y + m.headerH / 2, Align::Centre, TextStyleId::Caption, p.text2, c.header);
        else
        {
            dl.text(cx, r.y + m.headerH / 2 - 6, Align::Centre, TextStyleId::Caption, p.text2, c.header);
            dl.text(cx, r.y + m.headerH / 2 + 10, Align::Centre, TextStyleId::Caption, p.text3, c.sub);
        }
    }
    // Scrollbar: a 4 lpx thumb over the rows' height (the whole height while every row fits).
    const int n = count(m);
    const float top = r.y + m.headerH + 4, h = r.h - m.headerH - 8;
    const float frac = n <= m.visible ? 1.0f : static_cast<float>(m.visible) / static_cast<float>(n);
    const float off = n <= m.visible ? 0.0f : static_cast<float>(m.first) / static_cast<float>(n) * h;
    const float x = m.scrollbarX > 0 ? m.scrollbarX : r.right() - 8;
    dl.fill({x, top + off, 4, std::max(24.0f, h * frac)}, p.borderStrong, 2);
}

RectF DataTable::drawRow(DrawList& dl, const UiContext& ctx, const DataTableModel& m, int row)
{
    const auto& p = ctx.palette;
    const RectF rr {m.panel.x, m.panel.y + m.headerH + m.rowH * static_cast<float>(row - m.first), m.panel.w, m.rowH};
    dl.fill(rr, (row & 1) == 0 ? p.surface2 : p.surface1);
    if (row == m.focused)
        dl.fill(rr, p.accentWeak);
    if (row == m.playing && row < m.rows)
    {
        dl.fill({rr.x, rr.y, 3, rr.h}, p.play);
        // ▶ 10 × 14 at x + 14, centred on the row (arranger.md "No." column).
        for (int k = 0; k < 14; ++k)
        {
            const float w = 10.0f * (1.0f - std::fabs(static_cast<float>(k) + 0.5f - 7.0f) / 7.0f);
            dl.fill({rr.x + 14, rr.y + 15 + static_cast<float>(k), w, 1}, p.play);
        }
    }
    if (row == m.focused)
        dl.fill({rr.x, rr.y, 3, rr.h}, p.accent);
    return rr;
}

void DataTable::drawTerminal(DrawList& dl, const UiContext& ctx, const DataTableModel& m, RectF rr)
{
    const auto& p = ctx.palette;
    const float cx = m.terminalCentre > 0 ? m.terminalCentre : rr.x + rr.w / 2, cy = rr.y + rr.h / 2;
    const float half = ctx.measure ? ctx.measure(TextStyleId::Label, m.terminalText) / 2 : 70.0f;
    const float gapX = half + 28; // the rules stop 28 lpx short of the label
    dl.fill({cx - gapX - 80, cy - 0.5f, 80, 1}, p.borderStrong);
    dl.fill({cx + gapX, cy - 0.5f, 80, 1}, p.borderStrong);
    dl.text(cx, cy, Align::Centre, TextStyleId::Label, p.text2,
            m.terminalText);
}

void DataTable::cell(DrawList& dl, const DataTableModel& m, const theme::tk::Palette& p, RectF rr, int c,
                     std::string_view text, std::uint32_t ink, bool strong, float dx, Align align)
{
    if (c < 0 || c >= static_cast<int>(m.columns.size()))
        return;
    const auto& col = m.columns[static_cast<std::size_t>(c)];
    const float x = align == Align::Centre ? rr.x + col.x + col.w / 2 + dx : rr.x + col.x + dx;
    dl.text(x, rr.y + rr.h / 2, align, strong ? TextStyleId::BodyStrong : TextStyleId::Body, ink != 0 ? ink : p.text1,
            text);
}

int DataTable::scrollFor(int focused, int first, int n, int visible) noexcept
{
    if (n <= visible)
        return 0;
    if (focused < first + 1)
        first = focused - 1;
    if (focused > first + visible - 2)
        first = focused - visible + 2;
    return std::clamp(first, 0, n - visible);
}

int DataTable::rowAt(const DataTableModel& m, float x, float y) noexcept
{
    const float top = m.panel.y + m.headerH;
    if (x < m.panel.x || x >= m.panel.right() || y < top || y >= m.panel.bottom())
        return -1;
    const int row = m.first + static_cast<int>((y - top) / m.rowH);
    return row < count(m) && row - m.first < m.visible ? row : -1;
}
} // namespace ui
