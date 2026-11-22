#include "ui/widgets/StepGrid.h"

#include "ui/theme/Draw.h"

#include <cstdio>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
constexpr const char* kNumbers[] = {"1", "2",  "3",  "4",  "5",  "6",  "7",  "8",
                                    "9", "10", "11", "12", "13", "14", "15", "16"};
} // namespace

int StepGrid::stepAt(float x, float y) noexcept
{
    if (y < kCellY - 8 || y >= kCellY + kCellH + 8 || x < kCellX0 - 8)
        return 0;
    const int n = static_cast<int>((x - kCellX0 + 8) / kPitch) + 1;
    return n >= 1 && n <= 16 ? n : 0;
}

void StepGrid::draw(DrawList& dl, const UiContext& ctx, const StepGridModel& m)
{
    const auto& p = ctx.palette;
    dl.fill(kPanel, p.surface1, tk::radiusM);
    const float dim = m.muted ? 0.4f : 1.0f;
    const auto colour = theme::withOpacity(m.colour, dim);
    for (int n = 1; n <= 16; ++n)
    {
        const auto r = cell(n);
        const float cx = r.x + r.w / 2;
        const bool boxed = (n - 1) % 4 == 0; // 1, 5, 9, 13 (UID-751)
        if (boxed)
            dl.stroke({cx - 14, 174, 28, 20}, p.text2, 1, tk::radiusXs);
        dl.text(cx, 184, Align::Centre, TextStyleId::Caption, boxed ? p.text1 : p.text2, kNumbers[n - 1]);
        const bool target = (m.targets >> (n - 1) & 1u) != 0;
        if (target)
            dl.fill({r.x - 4, r.y - 4, r.w + 8, r.h + 8}, p.accentWeak, tk::radiusM);
        dl.fill(r, theme::withOpacity(p.surface3, dim), tk::radiusS);
        const auto& c = m.cells[static_cast<std::size_t>(n - 1)];
        // A locked step (any p-lock) in a lighter tint of the track colour, as on the step keys.
        const auto cellColour = c.locked ? theme::withOpacity(0xFF000000u | theme::lockedTint(m.colour), dim) : colour;
        switch (c.kind)
        {
        case StepKind::Full:
            dl.fill(r, cellColour, tk::radiusS);
            break;
        case StepKind::Half: // OQ-09: HALF drawn as the bottom half
            dl.fill({r.x, r.y + r.h / 2, r.w, r.h / 2}, cellColour, tk::radiusS);
            break;
        case StepKind::Param:
            dl.stroke({r.x + 1, r.y + 1, r.w - 2, r.h - 2}, cellColour, 2, tk::radiusS);
            dl.fill({cx - 4, r.y + r.h / 2 - 4, 8, 8}, p.mod, 4);
            break;
        case StepKind::Empty:
            break;
        }
        if (c.kind != StepKind::Empty && c.retrigCount > 1)
        {
            char buf[16];
            std::snprintf(buf, sizeof buf, "\xC3\x97%d", c.retrigCount);
            dl.text(cx, r.y + 18, Align::Centre, TextStyleId::Caption, c.kind == StepKind::Param ? p.text1 : p.onAccent,
                    buf);
        }
        if (n == m.playStep)
            dl.stroke({r.x - 3, r.y - 3, r.w + 6, r.h + 6}, p.playhead, 2, tk::radiusM);
        if (n == m.heldStep)
            draw::focusRing(dl, p, r, tk::radiusS);
    }
}
} // namespace ui
