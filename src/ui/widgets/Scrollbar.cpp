#include "ui/widgets/Scrollbar.h"

#include <algorithm>

namespace ui
{
void Scrollbar::draw(DrawList& dl, const UiContext& ctx, RectF t, const ScrollbarModel& m)
{
    if (m.total <= m.visible || m.visible <= 0)
        return; // shown only when the content overflows
    const float w = m.hover ? 8.0f : 4.0f;
    const float th = std::max(16.0f, t.h * m.visible / m.total);
    const float range = std::max(1.0f, m.total - m.visible);
    const float y = t.y + (t.h - th) * std::clamp(m.first / range, 0.0f, 1.0f);
    dl.fill({t.x + 4 - w, y, w, th}, ctx.palette.borderStrong, w / 2);
}
} // namespace ui
