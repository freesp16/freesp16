// Shared drawing primitives of the component library (07-ui-design/components/): the focus ring (UID-150), hairlines,
// the elevation-2 popup panel with its title band and ✕ (popups-and-feedback.md "Layout"), the scrim (overlay token),
// and the section caption on a hairline. Every colour comes from the palette passed in; sizes from the tokens.
#pragma once

#include "ui/render/DrawList.h"
#include "ui/theme/Theme.h"

#include <string_view>

namespace ui::draw
{
namespace tk = ::theme::tokens;

inline void controlFace(DrawList& dl, const tk::Palette& p, RectF r, std::uint32_t fill, float radius)
{
    dl.fill(r, fill, p.classic ? 2 : radius);
    if (p.classic)
        dl.stroke({r.x + 0.75f, r.y + 0.75f, r.w - 1.5f, r.h - 1.5f}, p.border, 1.5f, 2);
}

// 2 lpx accent ring, 2 lpx outside the item, radius = item radius + 2 (UID-150; mock.py focus()).
inline void focusRing(DrawList& dl, const tk::Palette& p, RectF r, float itemRadius)
{
    if (p.classic)
    {
        const float x = r.x + 1, y = r.y + 1, w = r.w - 2, h = r.h - 2;
        const float cut = std::min(16.0f, h / 3);
        dl.fill({x, y, w, 3}, p.text1);
        dl.fill({x, y, 3, h}, p.text1);
        dl.fill({x + w - 3, y, 3, h - cut + 1}, p.text1);
        dl.fill({x, y + h - 3, w - cut + 1, 3}, p.text1);
        for (int i = 0; i < static_cast<int>(cut); ++i)
            dl.fill({x + w - cut - 1 + i, y + h - 3 - i, 3, 3}, p.text1);
        return;
    }
    const float o = tk::sizeFocusRingOffset + tk::borderFocus * 0.5f + 1; // ring centre 4 lpx outside the edge
    dl.stroke({r.x - o, r.y - o, r.w + 2 * o, r.h + 2 * o}, p.accent, tk::borderFocus,
              itemRadius + tk::sizeFocusRingOffset);
}

inline void hline(DrawList& dl, float x0, float x1, float y, std::uint32_t c)
{
    dl.fill({x0, y - 0.5f, x1 - x0, 1}, c);
}
inline void vline(DrawList& dl, float x, float y0, float y1, std::uint32_t c)
{
    dl.fill({x - 0.5f, y0, 1, y1 - y0}, c);
}

// The scrim behind modals (overlay token, 72 % bg) over `r` (UID-125, UID-900).
inline void scrim(DrawList& dl, const tk::Palette& p, RectF r)
{
    dl.fill(r, p.overlay);
}

// Elevation-2 panel: drop shadow, surface-1 body radius 12, 1 lpx border (popups-and-feedback.md "Placement rule").
void panel(DrawList&, const tk::Palette&, RectF r);
// Title band of `bandH` (48 lists/messages, 56 modal/dialog/grid) with the title at x + 24 (or centred) and the ✕ at
// x + w − 28 (UID-900). A hairline closes modal bands.
void titleBand(DrawList&, const tk::Palette&, RectF r, float bandH, std::string_view title, bool centred, bool close,
               bool hairline);
// An 11 lpx caption centred on a hairline (── SLOPE ──, Modal sections); `measure` is the caption width in lpx.
void captionLine(DrawList&, const tk::Palette&, float x, float y, float w, std::string_view caption, float captionW,
                 std::uint32_t background);
} // namespace ui::draw
