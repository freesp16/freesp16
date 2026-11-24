#include "ui/widgets/OptionSelector.h"

#include "ui/theme/Draw.h"
#include "ui/theme/Glyphs.h"

namespace ui
{
namespace tk = ::theme::tokens;

void OptionSelector::draw(DrawList& dl, const UiContext& ctx, RectF r, const OptionSelectorModel& m)
{
    const auto& p = ctx.palette;
    draw::controlFace(dl, p, r, m.pressed ? p.surface3 : p.surface2, tk::radiusM);
    dl.text(r.x + tk::space[4], r.y + 18, Align::Left, TextStyleId::Caption, m.disabled ? p.text3 : p.text2, m.caption);
    const RectF box{r.x + tk::space[3], r.y + r.h - 48, r.w - 2 * tk::space[3], 36};
    dl.fill(box, p.surface1, tk::radiusS);
    dl.text(r.x + tk::space[5], box.y + 18, Align::Left, TextStyleId::BodyStrong, m.disabled ? p.text3 : p.text1,
            m.value);
    glyph::draw(dl, glyph::Kind::ChevronDown, r.x + r.w - 28 - 10, box.y + 18 - 10, 20, p.text3);
    if (m.focused)
        draw::focusRing(dl, p, r, tk::radiusM);
}
} // namespace ui
