#include "ui/widgets/Button.h"

#include "ui/theme/Draw.h"

#include <algorithm>

namespace ui
{
namespace tk = ::theme::tokens;

void Button::draw(DrawList& dl, const UiContext& ctx, RectF r, const ButtonModel& m)
{
    const auto& p = ctx.palette;
    const bool primary = m.style == ButtonStyle::Primary && ! m.disabled;
    const auto fill = m.disabled ? p.surface1 : (primary ? p.accent : (m.pressed ? p.surface3 : p.surface2));
    dl.fill(r, fill, tk::radiusS);
    const auto ink =
        m.disabled ? p.text3 : (primary ? p.onAccent : (m.style == ButtonStyle::Destructive ? p.rec : p.text1));
    const float cx = r.x + r.w / 2, cy = r.y + r.h / 2 + 1;
    if (m.line2.empty())
        dl.text(cx, cy, Align::Centre, TextStyleId::Label, ink, m.label);
    else
    {
        dl.text(cx, cy - 9, Align::Centre, TextStyleId::Label, ink, m.label);
        dl.text(cx, cy + 9, Align::Centre, TextStyleId::Label, ink, m.line2);
    }
    if (m.focused)
        draw::focusRing(dl, p, r, tk::radiusS);
}

RectF hitArea(RectF v) noexcept
{
    const float min = tk::sizeHitMin;
    const float w = std::max(v.w, min), h = std::max(v.h, min);
    return {v.x - (w - v.w) / 2, v.y - (h - v.h) / 2, w, h};
}
} // namespace ui
