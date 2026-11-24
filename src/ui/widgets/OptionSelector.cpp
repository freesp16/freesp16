#include "ui/widgets/OptionSelector.h"

#include "ui/theme/Draw.h"
#include "ui/theme/Glyphs.h"
#include "ui/widgets/TrackTile.h"

namespace ui
{
namespace tk = ::theme::tokens;

void OptionSelector::draw(DrawList& dl, const UiContext& ctx, RectF r, const OptionSelectorModel& m)
{
    const auto& p = ctx.palette;
    dl.fill(r, m.pressed ? p.surface3 : p.surface2, tk::radiusM);
    dl.text(r.x + tk::space[4], r.y + 18, Align::Left, TextStyleId::Caption, m.disabled ? p.text3 : p.text2, m.caption);
    const RectF box{r.x + tk::space[3], r.y + r.h - 48, r.w - 2 * tk::space[3], 36};
    dl.fill(box, p.surface1, tk::radiusS);
    // A long value (JAPANESE MIYAKOBUSHI) steps down to the label style, then middle-truncates, before the chevron.
    const float room = r.x + r.w - 28 - 10 - 4 - (r.x + tk::space[5]);
    auto style = TextStyleId::BodyStrong;
    std::string_view value = m.value;
    char fit[64];
    if (ctx.measure && ctx.measure(style, value) > room)
    {
        style = TextStyleId::Label;
        if (ctx.measure(style, value) > room)
            value = TrackTile::middleTruncate(value, room, style, ctx.measure, fit, sizeof fit);
    }
    dl.text(r.x + tk::space[5], box.y + 18, Align::Left, style, m.disabled ? p.text3 : p.text1, value);
    glyph::draw(dl, glyph::Kind::ChevronDown, r.x + r.w - 28 - 10, box.y + 18 - 10, 20, p.text3);
    if (m.focused)
        draw::focusRing(dl, p, r, tk::radiusM);
}
} // namespace ui
