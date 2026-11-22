#include "ui/widgets/KnobStrip.h"

#include "ui/shell/Layout.h"
#include "ui/shell/Timing.h"
#include "ui/theme/Draw.h"

#include <cstdio>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
namespace L = layout;
} // namespace

KnobStrip::KnobStrip() noexcept = default;

RectF KnobStrip::bounds() const noexcept
{
    return L::knobStrip;
}

void KnobStrip::build(DrawList& dl, const UiContext& ctx) const
{
    const auto& p = ctx.palette;
    dl.fill(L::knobStrip, p.surface1);
    draw::hline(dl, 0, L::canvasW, L::stripY, p.border);

    // StripTab: trapezoid, top edge 0–224, bottom 0–248 (slanted right edge), drawn as 2 lpx bands.
    const bool step = model_.stepHeld > 0;
    const auto fill = step ? p.mod : p.surface2;
    dl.fill({0, L::stripY, L::tabTopW, L::tabH}, fill);
    for (int k = 0; k < static_cast<int>(L::tabH) / 2; ++k)
        dl.fill({L::tabTopW, L::stripY + 2.0f * static_cast<float>(k), 2.0f * static_cast<float>(k) + 1.0f, 2}, fill);
    char tab[16];
    std::string_view label = model_.tab;
    if (step)
    {
        const int n = std::snprintf(tab, sizeof tab, "STEP %d", model_.stepHeld);
        label = std::string_view(tab, static_cast<std::size_t>(n > 0 ? n : 0));
    }
    dl.text(tk::space[4], L::stripY + 13, Align::Left, TextStyleId::Caption, step ? p.onAccent : p.text2, label);

    for (int n = 1; n < 6; ++n)
        if (model_.groupMask & (1u << n))
            draw::vline(dl, L::columnX(n), L::separatorY0, L::separatorY1, p.borderStrong);
    for (int n = 0; n < 6; ++n)
        ParamColumn::draw(dl, ctx, n, model_.columns[static_cast<std::size_t>(n)]);
}

bool KnobStrip::dirty(double nowMs) const
{
    return nowMs - lastTouchMs < timing::recentlyTouchedMs + 20;
}

int KnobStrip::columnAt(float x, float y) const noexcept
{
    if (y < L::stripY + L::tabH || y >= L::canvasH || x < L::columnX0)
        return -1;
    const int n = static_cast<int>((x - L::columnX0) / L::columnW);
    return n >= 0 && n < 6 ? n : -1;
}
} // namespace ui
