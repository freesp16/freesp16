#include "ui/widgets/SceneKey.h"

#include "ui/theme/Draw.h"

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
constexpr const char* kNumbers[] = {"1", "2",  "3",  "4",  "5",  "6",  "7",  "8",
                                    "9", "10", "11", "12", "13", "14", "15", "16"};
} // namespace

void SceneKey::draw(DrawList& dl, const UiContext& ctx, RectF r, const SceneKeyModel& m)
{
    const auto& p = ctx.palette;
    dl.fill(r, p.surface2, tk::radiusS);
    if (m.current && ! m.disabled)
        dl.fill(r, theme::withOpacity(m.colour, 0.2f), tk::radiusS);
    const RectF led{r.x + 8, r.y + 6, r.w - 16, 4};
    if (m.disabled)
        dl.fill(led, p.border, 2);
    else if (m.pending)
    {
        dl.fill(led, theme::withOpacity(m.colour, 0.5f), 2);
        dl.fill({led.x, led.y, led.w / 2, led.h}, m.colour, 2);
    }
    else
        dl.fill(led, m.current ? m.colour : (m.hasContent ? p.text2 : p.border), 2);
    const auto ink = m.disabled ? p.text3 : (m.hasContent || m.current ? p.text1 : p.text2);
    dl.text(r.x + r.w / 2, r.y + r.h / 2 + 6, Align::Centre, TextStyleId::ValueS, ink, kNumbers[(m.index - 1) & 15]);
    if (m.hasContent && ! m.current && ! m.disabled)
        dl.fill({r.x + r.w / 2 - 3, r.y + r.h - 12, 6, 4}, m.colour, 2);
    if (m.copied)
    {
        dl.fill({r.x + r.w - 20, r.y + r.h - 20, 14, 14}, p.surface3, 3);
        dl.stroke({r.x + r.w - 17, r.y + r.h - 17, 7, 7}, p.text2, 1.5f, 1);
    }
    if (m.trail)
        dl.stroke({r.x - 1, r.y - 1, r.w + 2, r.h + 2}, p.borderStrong, 2, tk::radiusS + 1);
    if (m.focused)
        draw::focusRing(dl, p, r, tk::radiusS);
}
} // namespace ui
