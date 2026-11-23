#include "ui/widgets/ChannelStrip.h"

#include "ui/theme/Draw.h"
#include "ui/theme/Glyphs.h"
#include "ui/widgets/Meter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
constexpr float kPi = 3.14159265f;
} // namespace

void ChannelStrip::arc(DrawList& dl, float cx, float cy, float r, float a0, float a1, float width, std::uint32_t c)
{
    if (a1 < a0)
        std::swap(a0, a1);
    const float len = (a1 - a0) * r, h = width * 0.5f;
    const int n = std::max(1, static_cast<int>(std::ceil(len / 1.5f)));
    for (int i = 0; i <= n; ++i)
    {
        const float a = a0 + (a1 - a0) * static_cast<float>(i) / static_cast<float>(n);
        dl.fill({cx + r * std::cos(a) - h, cy + r * std::sin(a) - h, width, width}, c, h);
    }
}

void ChannelStrip::miniRing(DrawList& dl, const UiContext& ctx, float cx, float cy, float v, bool bipolar,
                            std::uint32_t colour, std::string_view letter, bool dim)
{
    const auto& p = ctx.palette;
    constexpr float r = 12;
    dl.fill({cx - r, cy - r, 2 * r, 2 * r}, p.surface3, r);
    const float a0 = kPi * 0.75f, a1 = kPi * 2.25f;
    const float st = bipolar ? kPi * 1.5f : a0, en = a0 + (a1 - a0) * std::clamp(v, 0.0f, 1.0f);
    if (std::fabs(en - st) > 0.02f)
        arc(dl, cx, cy, r, st, en, 3, dim ? theme::withOpacity(colour, 0.4f) : colour);
    dl.text(cx, cy + 1, Align::Centre, TextStyleId::Caption, dim ? p.text3 : p.text2, letter);
}

void ChannelStrip::draw(DrawList& dl, const UiContext& ctx, const ChannelStripModel& m)
{
    const auto& p = ctx.palette;
    const auto r = rect(m.track);
    const float x = r.x, cx = x + kW / 2;
    const bool output = m.variant == ChannelStripModel::Variant::Output;
    const bool assigned = m.outAssign != "--" && ! m.outAssign.empty();
    if (output)
    {
        dl.fill({x, 152, kW, 28}, p.surface2, tk::radiusXs);
        dl.text(cx, 167, Align::Centre, TextStyleId::Caption, assigned ? p.text1 : p.text3, m.outAssign);
    }
    else
        dl.text(cx, 162, Align::Centre, TextStyleId::Caption, assigned ? p.text2 : p.text3, m.outAssign);
    const bool dim = m.mute;
    if (! output && ! m.midi)
        miniRing(dl, ctx, cx, 192, m.send01, false, m.colour, "S", dim);
    miniRing(dl, ctx, cx, 228, 0.5f + 0.5f * std::clamp(m.pan, -1.0f, 1.0f), true, m.colour, "P", dim);
    if (! output)
    {
        const struct
        {
            std::string_view label;
            bool on;
            std::uint32_t c;
            float y;
        } toggles[] = {{"MUTE", m.mute, p.accent, 252}, {"SOLO", m.solo, p.warn, 300}};
        for (const auto& t : toggles)
        {
            dl.fill({x, t.y, kW, 44}, p.surface2, tk::radiusS);
            dl.fill({x + 8, t.y + 6, kW - 16, 3}, t.on ? t.c : p.borderStrong, 1.5f);
            dl.text(cx, t.y + 26, Align::Centre, TextStyleId::Caption, t.on ? p.text1 : p.text2, t.label);
        }
    }
    MeterModel mm;
    for (int ch = 0; ch < 2; ++ch)
    {
        mm.db[ch] = m.mute ? -120.0f : m.levelDb[ch];
        mm.peakDb[ch] = m.mute ? -120.0f : m.peakDb[ch];
    }
    mm.faderDb = m.faderDb;
    mm.dim = dim || m.otherSolo;
    mm.markerOver = true;
    Meter::drawBars(dl, ctx, x + (kW - kBarsW) / 2, kMeterTop, kMeterBottom, 10, mm);

    const auto chip = m.hasContent ? m.colour : m.colourDim;
    const float chipR = x + kW - 4;
    dl.fill({x + 4, 572, kW - 8, 32}, m.mute ? theme::withOpacity(chip, 0.45f) : chip, tk::radiusXs);
    char number[4];
    std::snprintf(number, sizeof number, "%d", m.track);
    dl.text(cx, 589, Align::Centre, TextStyleId::BodyStrong, m.hasContent ? p.onAccent : p.text1, number);
    if (m.mute) // 10 lpx ✕ mute glyph on the chip
    {
        glyph::line(dl, chipR - 10, 576, chipR - 2, 584, 1.5f, p.text1);
        glyph::line(dl, chipR - 2, 576, chipR - 10, 584, 1.5f, p.text1);
    }
    if (m.selected) // one thin outline inside the strip (1 device px at x0.625): no ring over the neighbours
        dl.stroke({x + 0.8f, r.y + 0.8f, kW - 1.6f, r.h - 1.6f}, m.colour, 1.6f, tk::radiusS);
}
} // namespace ui
