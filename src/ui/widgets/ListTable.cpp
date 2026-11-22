#include "ui/widgets/ListTable.h"

#include "ui/theme/Draw.h"
#include "ui/theme/Glyphs.h"
#include "ui/widgets/Scrollbar.h"

#include <algorithm>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
constexpr float kX = 224, kW = 1040;

float textW(const UiContext& ctx, TextStyleId s, std::string_view t)
{
    return ctx.measure ? ctx.measure(s, t) : 8.6f * static_cast<float>(t.size());
}

// 1.5 lpx line icons on a 20 lpx grid (browse.py folder_icon / file_icon).
void folderIcon(DrawList& dl, float x, float y, bool open, std::uint32_t c)
{
    if (open)
    {
        glyph::line(dl, x, y + 4, x + 7, y + 4, 1.5f, c);
        glyph::line(dl, x + 7, y + 4, x + 9, y + 6, 1.5f, c);
        glyph::line(dl, x + 9, y + 6, x + 18, y + 6, 1.5f, c);
        glyph::line(dl, x, y + 4, x, y + 17, 1.5f, c);
        glyph::line(dl, x, y + 17, x + 16, y + 17, 1.5f, c);
        glyph::line(dl, x + 16, y + 17, x + 19, y + 8, 1.5f, c);
        glyph::line(dl, x + 19, y + 8, x + 3, y + 8, 1.5f, c);
        glyph::line(dl, x + 3, y + 8, x, y + 17, 1.5f, c);
        return;
    }
    glyph::line(dl, x, y + 4, x + 7, y + 4, 1.5f, c);
    glyph::line(dl, x + 7, y + 4, x + 9, y + 6, 1.5f, c);
    glyph::line(dl, x + 9, y + 6, x + 19, y + 6, 1.5f, c);
    glyph::line(dl, x + 19, y + 6, x + 19, y + 17, 1.5f, c);
    glyph::line(dl, x + 19, y + 17, x, y + 17, 1.5f, c);
    glyph::line(dl, x, y + 17, x, y + 4, 1.5f, c);
}

void fileIcon(DrawList& dl, float x, float y, std::uint32_t c)
{
    glyph::line(dl, x + 2, y, x + 12, y, 1.5f, c);
    glyph::line(dl, x + 12, y, x + 17, y + 5, 1.5f, c);
    glyph::line(dl, x + 17, y + 5, x + 17, y + 20, 1.5f, c);
    glyph::line(dl, x + 17, y + 20, x + 2, y + 20, 1.5f, c);
    glyph::line(dl, x + 2, y + 20, x + 2, y, 1.5f, c);
    glyph::line(dl, x + 5, y + 12, x + 7, y + 8, 1.5f, c);
    glyph::line(dl, x + 7, y + 8, x + 9, y + 14, 1.5f, c);
    glyph::line(dl, x + 9, y + 14, x + 11, y + 9, 1.5f, c);
    glyph::line(dl, x + 11, y + 9, x + 13, y + 12, 1.5f, c);
}
} // namespace

int ListTable::scrollFor(int focused, int first, int count) noexcept
{
    const int maxFirst = std::max(0, count - kVisible);
    if (focused < 0)
        return std::clamp(first, 0, maxFirst);
    if (focused < first + 1)
        first = focused - 1;
    if (focused > first + kVisible - 2)
        first = focused - kVisible + 2;
    return std::clamp(first, 0, maxFirst);
}

void ListTable::drawRow(DrawList& dl, const UiContext& ctx, float ry, int zebra, const TreeRowModel& r, bool focused,
                        bool previewing)
{
    const auto& p = ctx.palette;
    dl.fill({kX, ry, kW, kRowH}, zebra % 2 == 0 ? p.surface2 : p.surface1);
    if (focused)
    {
        dl.fill({kX, ry, kW, kRowH}, p.accentWeak);
        dl.fill({kX, ry, 3, kRowH}, p.accent);
    }
    if (r.kind == TreeRowModel::Kind::Message)
    {
        dl.text(kX + kW / 2, ry + 22, Align::Centre, TextStyleId::Body, r.disabled ? p.text3 : p.text2, r.name);
        return;
    }
    const float ix = kX + 24 + 24 * static_cast<float>(r.level);
    if (r.level > 0)
    {
        draw::vline(dl, ix - 14, ry - (zebra == 0 ? 0 : 8), ry + 22, p.borderStrong);
        draw::hline(dl, ix - 14, ix - 4, ry + 22, p.borderStrong);
    }
    const auto style = focused ? TextStyleId::BodyStrong : TextStyleId::Body;
    if (r.kind == TreeRowModel::Kind::File)
    {
        fileIcon(dl, ix, ry + 12, r.assigned ? p.accent : p.text2);
        dl.text(ix + 28, ry + 22, Align::Left, style, r.assigned ? p.accent : (r.disabled ? p.text3 : p.text1), r.name);
        if (r.assigned)
            glyph::draw(dl, glyph::Kind::Check, kX + kW - 80 - 8, ry + 14, tk::sizeIconS, p.accent);
        if (focused && previewing)
            dl.text(ix + 28 + textW(ctx, style, r.name) + 16, ry + 22, Align::Left, TextStyleId::Caption, p.play,
                    "\xE2\x96\xB6 PREVIEW");
        if (! r.value.empty())
            dl.text(kX + 520, ry + 22, Align::Left, TextStyleId::Body, p.text2, r.value);
        if (r.usb)
            dl.text(kX + kW - 28, ry + 22, Align::Right, TextStyleId::Caption, p.text3, "USB");
        return;
    }
    const bool open = r.kind == TreeRowModel::Kind::OpenFolder;
    dl.text(ix - 2, ry + 22, Align::Right, TextStyleId::Body, p.text3, open ? "\xE2\x96\xBE" : "\xE2\x96\xB8");
    folderIcon(dl, ix + 2, ry + 11, open, p.text2);
    dl.text(ix + 32, ry + 22, Align::Left, style, r.disabled ? p.text3 : p.text1, r.name);
    if (! r.badge.empty())
    {
        const float bx = ix + 32 + textW(ctx, style, r.name) + 12, bw = textW(ctx, TextStyleId::Caption, r.badge) + 16;
        dl.fill({bx, ry + 11, bw, 22}, p.surface3, tk::radiusXs);
        dl.text(bx + bw / 2, ry + 23, Align::Centre, TextStyleId::Caption, p.text2, r.badge);
    }
}

void ListTable::draw(DrawList& dl, const UiContext& ctx, const ListTableModel& m)
{
    const auto& p = ctx.palette;
    dl.fill(kPanel, p.surface1, tk::radiusM);
    dl.fill({kX, 152, kW, 48}, p.surface2, tk::radiusM);
    dl.fill({kX, 188, kW, 12}, p.surface2);
    dl.text(kX + 24, 176, Align::Left, TextStyleId::Title, p.text1, m.title);
    dl.text(kX + kW - 24, 176, Align::Right, TextStyleId::Caption, p.text2, m.sort);
    dl.text(kX + kW - 80, 176, Align::Right, TextStyleId::Caption, p.text3, m.freeSpace);
    const int count = static_cast<int>(m.rows.size());
    for (int i = 0; i < kVisible; ++i)
    {
        const float ry = kRowY0 + kRowH * static_cast<float>(i);
        const int k = m.first + i;
        if (k >= count)
        {
            dl.fill({kX, ry, kW, kRowH}, i % 2 == 0 ? p.surface2 : p.surface1);
            continue;
        }
        drawRow(dl, ctx, ry, i, m.rows[static_cast<std::size_t>(k)], k == m.focused, m.previewing);
        if (k == m.loadingRow && m.loading >= 0) // inline load progress, never blocking (UID-686)
        {
            const float x0 = kX + 24 + 24 * static_cast<float>(m.rows[static_cast<std::size_t>(k)].level) + 28;
            dl.fill({x0, ry + 36, 400, 4}, p.surface3, 2);
            dl.fill({x0, ry + 36, std::max(4.0f, 400 * std::min(1.0f, m.loading)), 4}, p.accent, 2);
        }
    }
    if (count > kVisible)
        Scrollbar::draw(dl, ctx, {kX + kW - 8, 204, 4, 396},
                        {static_cast<float>(count), static_cast<float>(kVisible), static_cast<float>(m.first)});
}
} // namespace ui
