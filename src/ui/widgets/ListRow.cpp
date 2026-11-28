#include "ui/widgets/ListRow.h"

#include "ui/theme/Draw.h"
#include "ui/theme/Glyphs.h"

namespace ui
{
namespace tk = ::theme::tokens;

void ListRow::draw(DrawList& dl, const UiContext& ctx, RectF r, const ListRowModel& m)
{
    const auto& p = ctx.palette;
    const float cy = r.y + r.h / 2;
    if (m.header)
    {
        dl.fill(r, p.surface3);
        dl.text(r.x + tk::space[4], cy, Align::Left, p.classic ? TextStyleId::Value : TextStyleId::Caption,
                p.classic ? p.text1 : p.text2, m.label);
        return;
    }
    dl.fill(r, m.zebra % 2 == 0 ? p.surface2 : p.surface1);
    if (p.classic)
        draw::hline(dl, r.x, r.right(), r.bottom() - 1, p.border);
    const bool split = p.classic && m.valueX >= 0;
    if (m.focused)
    {
        dl.fill(r, p.accentWeak);
        if (split)
            dl.fill({r.x, r.y, m.valueX - 16, r.h}, p.text1);
        dl.fill({r.x, r.y, 3, r.h}, p.accent);
    }
    const auto ink = m.disabled ? p.text3 : (m.current ? p.accent : p.text1);
    const auto labelStyle = m.focused ? TextStyleId::BodyStrong : TextStyleId::Body;
    dl.text(r.x + m.labelX + m.indent, cy, Align::Left, split ? TextStyleId::Value : labelStyle,
            split && m.focused ? p.onAccent : ink, m.label);
    const float vx = r.x + (m.valueX >= 0 ? m.valueX : r.w / 2);
    if (m.editing)
    {
        const RectF cell{vx - 8, r.y + 4, r.x + r.w - (vx - 8) - 8, r.h - 8};
        dl.fill(cell, p.surface1, tk::radiusS);
        dl.stroke(cell, p.accent, 1.5f, tk::radiusS);
        dl.text(cell.x + 16, cy, Align::Centre, TextStyleId::Title, p.accent, "\xE2\x80\xB9");
        dl.text(cell.x + 40, cy, Align::Left, split ? TextStyleId::Value : TextStyleId::BodyStrong, p.text1, m.value);
        dl.text(cell.x + cell.w - 16, cy, Align::Centre, TextStyleId::Title, p.accent, "\xE2\x80\xBA");
    }
    else if (! m.value.empty())
        dl.text(vx, cy, Align::Left, TextStyleId::Body, m.disabled ? p.text3 : p.text2, m.value);
    if (m.current)
        glyph::draw(dl, glyph::Kind::Check, r.x + r.w - 32 - 10, cy - 10, 20, p.accent);
    else if (m.opens)
        dl.text(r.x + r.w - 24, cy, Align::Centre, TextStyleId::Title, p.text3, "\xE2\x80\xBA");
}
} // namespace ui
