#include "ui/widgets/BigValueField.h"

#include "ui/theme/Draw.h"

#include <algorithm>
#include <cmath>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;

// The stepper is drawn while the field is focused and editable (the mockup shows it on the focused BPM field
// only; the unfocused SWING field has none).
bool stepperShown(const BigValueModel& m) noexcept
{
    return m.editable && m.focused && ! m.disabled;
}
} // namespace

void BigValueField::draw(DrawList& dl, const UiContext& ctx, RectF r, const BigValueModel& m)
{
    const auto& p = ctx.palette;
    dl.fill(r, p.surface2, tk::radiusM);
    const float cy = r.y + 84;
    const float right = r.x + m.split;
    dl.text(right, cy, Align::Right, TextStyleId::Display, m.disabled ? p.text2 : p.text1, m.whole);
    if (! m.fraction.empty())
        dl.text(right + m.fractionGap, cy + m.fractionDy, Align::Left, TextStyleId::ValueL, p.text2, m.fraction);
    if (m.disabled && ! m.badge.empty())
    {
        const float w = (ctx.measure ? ctx.measure(TextStyleId::Caption, m.badge) : 32.0f) + 16;
        dl.fill({r.x + 12, r.y + 12, w, 22}, p.surface3, tk::radiusXs);
        dl.text(r.x + 12 + w / 2, r.y + 23, Align::Centre, TextStyleId::Caption, p.text1, m.badge);
    }
    if (stepperShown(m))
    {
        // A 2 lpx arc of radius 64 from (x + 280, y + 24) to (x + 280, y + 136), bulging right (mockup path).
        const float ax = r.x + 249, ay = r.y + 80, rad = 64;
        const float a0 = -std::asin(56.0f / 64.0f);
        // 24 chords, each drawn as its 2 lpx-padded bounding box (24 commands instead of ~140 dots).
        constexpr int kSegments = 24;
        float px = ax + rad * std::cos(a0), py = ay + rad * std::sin(a0);
        for (int i = 1; i <= kSegments; ++i)
        {
            const float a = a0 + (-2 * a0) * static_cast<float>(i) / kSegments;
            const float qx = ax + rad * std::cos(a), qy = ay + rad * std::sin(a);
            dl.fill({std::min(px, qx) - 1, std::min(py, qy) - 1, std::abs(qx - px) + 2, std::abs(qy - py) + 2},
                    p.borderStrong);
            px = qx;
            py = qy;
        }
        dl.text(r.x + 304, r.y + 42, Align::Centre, TextStyleId::Title, p.text2, "+");
        dl.text(r.x + 304, r.y + 80, Align::Centre, TextStyleId::Title, m.focused ? p.accent : p.text2, "\xE2\x86\x95");
        dl.text(r.x + 304, r.y + 118, Align::Centre, TextStyleId::Title, p.text2, "\xE2\x88\x92");
    }
    if (m.focused)
        draw::focusRing(dl, p, r, tk::radiusM);
}

int BigValueField::stepAt(RectF r, const BigValueModel& m, float x, float y) noexcept
{
    if (! stepperShown(m) || x < r.x + r.w - 64 || x >= r.x + r.w || y < r.y || y >= r.y + r.h)
        return 0;
    return y < r.y + r.h / 2 ? 1 : -1;
}
} // namespace ui
