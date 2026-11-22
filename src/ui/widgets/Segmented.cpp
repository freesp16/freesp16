#include "ui/widgets/Segmented.h"

#include "ui/theme/Draw.h"
#include "ui/widgets/Button.h"

namespace ui
{
namespace tk = ::theme::tokens;

void Segmented::draw(DrawList& dl, const UiContext& ctx, RectF r, const SegmentedModel& m)
{
    const auto& p = ctx.palette;
    const auto n = static_cast<int>(m.items.size());
    if (n == 0)
        return;
    if (! m.caption.empty())
        draw::captionLine(dl, p, r.x, r.y - 14, r.w, m.caption,
                          ctx.measure ? ctx.measure(TextStyleId::Caption, m.caption) : 0,
                          m.captionBackground != 0 ? m.captionBackground : p.bg);
    const float sw = r.w / static_cast<float>(n);
    dl.fill(r, p.surface2, tk::radiusM);
    for (int i = 0; i < n; ++i)
    {
        const float sx = r.x + sw * static_cast<float>(i);
        if (i > 0)
            draw::vline(dl, sx, r.y + 8, r.y + r.h - 8, p.border);
        if (i == m.selected)
        {
            dl.fill({sx + 2, r.y + 2, sw - 4, r.h - 4}, p.accentWeak, tk::radiusS);
            dl.fill({sx + 12, r.y + 2, sw - 24, 3}, p.accent, 1);
        }
        const bool disabled = (m.disabledMask >> i) & 1u;
        dl.text(sx + sw / 2, r.y + r.h / 2 + 2, Align::Centre, TextStyleId::Label,
                disabled ? p.text3 : (i == m.selected ? p.text1 : p.text2), m.items[static_cast<std::size_t>(i)]);
    }
    if (m.focused)
        draw::focusRing(dl, p, r, tk::radiusM);
}

int Segmented::segmentAt(float x, float y) const noexcept
{
    const auto n = static_cast<int>(model_.items.size());
    if (n == 0 || ! contains(hitArea(rect_), x, y))
        return -1;
    const int i = static_cast<int>((x - rect_.x) / (rect_.w / static_cast<float>(n)));
    return i >= 0 && i < n && ! ((model_.disabledMask >> i) & 1u) ? i : -1;
}

bool Segmented::handle(const io::ControlEvent& e, UiContext&)
{
    if (e.control != io::Control::Selector || e.kind != io::ControlEvent::Kind::Turn || ! model_.focused)
        return false;
    const auto n = static_cast<int>(model_.items.size());
    int s = model_.selected;
    for (int k = 0; k < n; ++k) // next enabled segment in the turn direction, no wrap
    {
        const int t = s + (e.value > 0 ? 1 : -1);
        if (t < 0 || t >= n)
            break;
        s = t;
        if (! ((model_.disabledMask >> s) & 1u))
        {
            model_.selected = s;
            break;
        }
    }
    return true;
}
} // namespace ui
