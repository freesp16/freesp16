#include "ui/widgets/StripPreview.h"

#include "ui/theme/Theme.h"

#include <algorithm>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
}

RectF StripPreview::zone(int k) noexcept
{
    const int fromTop = 7 - std::clamp(k, 0, 7);
    return {kPanel.x + (kPanel.w - kZoneW) / 2, kZoneTop + kZonePitch * static_cast<float>(fromTop), kZoneW, kZoneH};
}

void StripPreview::draw(DrawList& dl, const UiContext& ctx, const StripPreviewModel& m)
{
    const auto& p = ctx.palette;
    const float cx = kPanel.x + kPanel.w / 2;
    dl.fill(kPanel, p.surface1, tk::radiusM);
    dl.text(cx, 172, Align::Centre, TextStyleId::Caption, p.text2, "STRIP PREVIEW");
    dl.text(cx, 204, Align::Centre, TextStyleId::Caption, p.text3, "TOP");
    dl.text(cx, 226, Align::Centre, TextStyleId::ValueS, m.empty ? p.text3 : p.text1, m.topText);
    const int live = m.livePosition >= 0 ? std::clamp(m.livePosition * 8 / 128, 0, 7) : -1;
    for (int k = 0; k < 8; ++k)
    {
        const auto z = zone(k);
        dl.fill(z, p.surface3, tk::radiusXs);
        if (m.empty && live < 0)
            continue;
        float a;
        if (live >= 0) // touched: the LEDs' bar graph up to the touched zone, strongest at the BOTTOM end
            a = k <= live ? std::min(1.0f, 0.25f * static_cast<float>(live - k + 1)) : 0.0f;
        else // the value the zone's centre maps to, as a share of the target's range
            a = std::clamp(m.bottom01 + (m.top01 - m.bottom01) * (static_cast<float>(k) + 0.5f) / 8.0f, 0.0f, 1.0f) * 0.8f;
        if (a > 0.02f)
            dl.fill(z, theme::withOpacity(p.accent, a), tk::radiusXs);
    }
    if (live >= 0) // 2 lpx playhead line on the touched zone's top edge
        dl.fill({cx - 32, zone(live).y - 4, 64, 2}, p.playhead);
    dl.text(cx, 564, Align::Centre, TextStyleId::Caption, p.text3, "BOTTOM");
    dl.text(cx, 586, Align::Centre, TextStyleId::ValueS, m.empty ? p.text3 : p.text1, m.bottomText);
}
} // namespace ui
