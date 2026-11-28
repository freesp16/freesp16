#include "ui/widgets/ParamColumn.h"

#include "ui/shell/Layout.h"
#include "ui/theme/Glyphs.h"
#include "ui/widgets/TrackTile.h"

#include <algorithm>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
namespace L = layout;
} // namespace

RectF ParamColumn::bounds() const noexcept
{
    return L::column(index_);
}

void ParamColumn::build(DrawList& dl, const UiContext& ctx) const
{
    draw(dl, ctx, index_, model_);
}

void ParamColumn::draw(DrawList& dl, const UiContext& ctx, int index0, const ParamColumnModel& c)
{
    if (c.spec == nullptr)
        return; // empty: nothing drawn (UID-003 keeps the column's place, UID-311 keeps the strip)
    const auto& p = ctx.palette;
    const auto& s = *c.spec;
    const float x = L::columnX(index0), cx = x + L::columnW / 2;
    const bool grey = c.disabled || c.inactive;
    if (c.locked) // 1.5 lpx mod outline inset 4 lpx
        dl.stroke({x + 4, L::stripY + 28, L::columnW - 8, 108}, p.mod, 1.5f, tk::radiusS);

    const float captionW = ctx.measure ? ctx.measure(TextStyleId::Caption, s.label) : 0;
    if (s.lockable) // 6 lpx mod dot before the caption (UID-014, UID-165)
        dl.fill({cx - captionW / 2 - 13, L::columnCaptionY - 3, 6, 6}, p.mod, 3);
    dl.text(cx, L::columnCaptionY, Align::Centre, TextStyleId::Caption, grey ? p.text3 : p.text2, s.label);
    if (c.locked) // 12 lpx lock glyph after the caption
        glyph::draw(dl, glyph::Kind::Lock, cx + captionW / 2 + 10, L::columnCaptionY - 5.5f, 12, p.mod);
    if (c.fine && ! c.disabled)
        dl.text(x + L::columnW - 8, L::columnCaptionY, Align::Right, TextStyleId::Caption, p.accent, "FINE");

    char buf[32];
    const int len = c.valueText.empty()
                        ? std::clamp(s.formatValue(c.value, buf, sizeof buf), 0, static_cast<int>(sizeof buf) - 1)
                        : 0;
    auto value = c.valueText.empty() ? std::string_view(buf, static_cast<std::size_t>(len)) : c.valueText;
    auto style = TextStyleId::Value;
    char shortened[256];
    const float available = L::columnW - 2.0f;
    if (! c.valueText.empty() && ctx.measure && ctx.measure(style, value) > available)
    {
        style = TextStyleId::ValueS;
        value = TrackTile::middleTruncate(value, available, style, ctx.measure, shortened, sizeof shortened);
    }
    dl.text(cx, L::columnValueY, Align::Centre, style, c.locked ? p.mod : (grey ? p.text3 : p.text1), value);
    if (c.touched)
        dl.fill({x + 64, L::columnUnderlineY, 80, 2}, p.accent, 1);
    if (! c.subReadout.empty())
        dl.text(cx, L::columnSubY, Align::Centre, TextStyleId::Caption, p.text2, c.subReadout);
    if (c.readOnly)
        return;

    // Value track 128 × 2 at x + 40 … x + 168.
    const float tx = x + 40, tw = 128, ty = L::columnTrackY;
    if (s.type == core::ParamType::Bool)
        return;
    dl.fill({tx, ty, tw, 2}, p.borderStrong, 1);
    if (s.type == core::ParamType::Enum || c.stepped)
    {
        dl.text(x + 36, ty + 1, Align::Centre, TextStyleId::Body, p.text3, "\xE2\x80\xB9");  // ‹
        dl.text(x + 172, ty + 1, Align::Centre, TextStyleId::Body, p.text3, "\xE2\x80\xBA"); // ›
        return;
    }
    const float t = s.normalised(c.value);
    const auto ink = grey ? p.text3 : p.text2;
    if (s.bipolar)
    {
        const float a = std::min(cx, tx + tw * t), b = std::max(cx, tx + tw * t);
        dl.fill({a, ty, std::max(2.0f, b - a), 2}, ink, 1);
        dl.fill({cx - 1, ty - 4, 2, 10}, ink); // 2 × 8 centre tick (mock: 2 × 10 from y 735)
    }
    else
        dl.fill({tx, ty, std::max(2.0f, tw * t), 2}, ink, 1);
}
} // namespace ui
