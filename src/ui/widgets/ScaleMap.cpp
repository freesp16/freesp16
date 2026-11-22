#include "ui/widgets/ScaleMap.h"

#include "core/util/scales/Scales.h"
#include "ui/theme/Theme.h"

#include <algorithm>
#include <cstdio>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
int floorMod(int a, int n) noexcept
{
    return ((a % n) + n) % n;
}
float flash(double nowMs, double sinceMs) noexcept // pad-flash: 1 → 0 over motion.pad-flash
{
    const double t = nowMs - sinceMs;
    return t >= 0 && t < tk::motionPadFlashMs ? static_cast<float>(1.0 - t / tk::motionPadFlashMs) : 0.0f;
}
} // namespace

RectF ScaleMap::windowFrame() noexcept
{
    const float wy = kY + static_cast<float>(kBaseRow - 3) * kPitch;
    return {kX - 4, wy - 4, 4 * kCellW + 3 * kGap + 8, 4 * kPitch + 4};
}

void ScaleMap::draw(DrawList& dl, const UiContext& ctx, const ScaleMapModel& m)
{
    const auto& p = ctx.palette;
    dl.fill(kPanel, p.surface1, tk::radiusM);
    const int n = core::scales::degreeCount(m.mask);
    const float f = flash(ctx.nowMs, m.flashSinceMs);
    for (int r = 0; r < 16; ++r)
        for (int c = 0; c < 4; ++c)
        {
            const int d = (kBaseRow - r) * 4 + c + core::scales::kPads * m.area;
            const RectF cell{kX + static_cast<float>(c) * (kCellW + kGap), kY + static_cast<float>(r) * kPitch, kCellW,
                             kCellH};
            if (d == 0)
                dl.fill(cell, p.text1, tk::radiusXs);
            else if (floorMod(d, n) == 0)
                dl.fill(cell, m.colour, tk::radiusXs);
            else
                dl.fill(cell, theme::withOpacity(m.colour, 0.35f), tk::radiusXs);
            if (d == m.flashDegree && f > 0)
                dl.fill(cell, theme::withOpacity(p.text1, 0.6f * f), tk::radiusXs);
        }
    dl.stroke(windowFrame(), p.text1, 2, tk::radiusS);
}

RectF PadMap::cell(int pad) noexcept
{
    const int r = 3 - pad / 4, c = pad % 4;
    return {kPanel.x + static_cast<float>(c) * (kCellW + kGap), kPanel.y + static_cast<float>(r) * (kCellH + kGap),
            kCellW, kCellH};
}

int PadMap::padAt(float x, float y) noexcept
{
    for (int pd = 0; pd < 16; ++pd)
    {
        const auto r = cell(pd);
        if (x >= r.x && x < r.right() && y >= r.y && y < r.bottom())
            return pd;
    }
    return -1;
}

void PadMap::draw(DrawList& dl, const UiContext& ctx, const PadMapModel& m)
{
    const auto& p = ctx.palette;
    static constexpr const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const float f = flash(ctx.nowMs, m.flashSinceMs);
    for (int pd = 0; pd < 16; ++pd)
    {
        const auto c = cell(pd);
        char lab[12];
        if (m.midi)
        {
            const int note = core::scales::midiPadNote(pd, m.mask, m.rootNote, m.area);
            std::snprintf(lab, sizeof lab, "%s%d", names[note % 12], note / 12);
        }
        else
        {
            const int semi = core::scales::padSemitones(pd, m.mask, m.keyShift, m.area);
            std::snprintf(lab, sizeof lab, semi > 0 ? "+%d" : "%d", semi);
        }
        const float cx = c.x + kCellW / 2, cy = c.y + kCellH / 2;
        if (pd == 0 && m.area == 0)
        {
            dl.fill(c, p.text1, tk::radiusM);
            for (int k = 0; k < 16; ++k) // accent corner triangle (root marker)
                dl.fill({c.x + (k < 8 ? 8.0f - static_cast<float>(k) : 0.0f), c.y + static_cast<float>(k),
                         24.0f - static_cast<float>(k) - (k < 8 ? 8.0f - static_cast<float>(k) : 0.0f), 1},
                        p.accent);
            dl.text(cx, cy, Align::Centre, TextStyleId::Value, p.onAccent, lab);
            dl.text(cx, c.y + kCellH - 16, Align::Centre, TextStyleId::Caption, p.onAccent, "ROOT \xC2\xB7 PAD 1");
        }
        else if (core::scales::isRootPad(pd, m.mask, m.area))
        {
            dl.fill(c, m.colour, tk::radiusM);
            dl.text(cx, cy, Align::Centre, TextStyleId::Value, p.onAccent, lab);
        }
        else
        {
            dl.fill(c, p.surface2, tk::radiusM);
            dl.fill(c, theme::withOpacity(m.colour, 0.35f), tk::radiusM);
            dl.text(cx, cy, Align::Centre, TextStyleId::Value, p.text1, lab);
        }
        if (pd == m.flashPad && f > 0) // pad-flash like a TrackTile hit (root-octave pads stay solid underneath)
            dl.fill(c, theme::withOpacity(p.text1, 0.3f * f), tk::radiusM);
    }
}
} // namespace ui
