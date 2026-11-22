#include "ui/widgets/Toggle.h"

#include "ui/theme/Draw.h"

namespace ui
{
namespace tk = ::theme::tokens;

void Toggle::draw(DrawList& dl, const UiContext& ctx, RectF r, const ToggleModel& m)
{
    const auto& p = ctx.palette;
    dl.fill(r, m.disabled ? p.surface1 : (m.pressed ? p.surface3 : p.surface2), tk::radiusS);
    const auto led = m.on ? (m.onColour != 0 ? m.onColour : p.accent) : (m.disabled ? p.border : p.borderStrong);
    dl.fill({r.x + tk::space[4], r.y + tk::space[2], r.w - 2 * tk::space[4], 4}, led, 2);
    dl.text(r.x + r.w / 2, r.y + r.h / 2 + 4, Align::Centre, TextStyleId::Label, m.disabled ? p.text3 : p.text1,
            m.label);
    if (m.focused)
        draw::focusRing(dl, p, r, tk::radiusS);
}
} // namespace ui
