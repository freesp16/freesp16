#include "ui/theme/Draw.h"

#include "ui/theme/Glyphs.h"

namespace ui::draw
{
void panel(DrawList& dl, const tk::Palette& p, RectF r)
{
    // Elevation 2: the token's drop shadow (offset y, 40 % black) drawn as one rect 1 lpx larger (mock.py panel()).
    dl.fill({r.x - 1, r.y + tk::elevation2OffsetY - 1, r.w + 2, r.h + 2},
            theme::withOpacity(tk::elevation2Shadow, (p.bg >> 24) / 255.0f), tk::radiusL + 1);
    dl.fill(r, p.surface1, tk::radiusL);
    dl.stroke(r, p.border, tk::elevation2Border, tk::radiusL);
}

void titleBand(DrawList& dl, const tk::Palette& p, RectF r, float bandH, std::string_view title, bool centred,
               bool close, bool hairline)
{
    dl.fill({r.x, r.y, r.w, bandH}, p.surface2, tk::radiusL);
    dl.fill({r.x, r.y + bandH - tk::radiusL, r.w, static_cast<float>(tk::radiusL)}, p.surface2);
    if (hairline)
        hline(dl, r.x, r.x + r.w, r.y + bandH, p.border);
    const float cy = r.y + bandH / 2;
    if (centred)
        dl.text(r.x + r.w / 2, cy, Align::Centre, TextStyleId::Title, p.text1, title);
    else
        dl.text(r.x + tk::space[5], cy, Align::Left, TextStyleId::Title, p.text1, title);
    if (close) // ✕ centred at x + w − 28 (a 44 × 44 hit area around it)
        glyph::draw(dl, glyph::Kind::Close, r.x + r.w - 28 - 12, cy - 12, tk::sizeIcon, p.text2);
}

void captionLine(DrawList& dl, const tk::Palette& p, float x, float y, float w, std::string_view caption,
                 float captionW, std::uint32_t background)
{
    hline(dl, x, x + w, y, p.border);
    const float cw = captionW + 20;
    dl.fill({x + w / 2 - cw / 2, y - 8, cw, 16}, background);
    dl.text(x + w / 2, y, Align::Centre, TextStyleId::Caption, p.text2, caption);
}
} // namespace ui::draw
