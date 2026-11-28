#include "ui/widgets/ModuleCard.h"

#include "ui/widgets/EffectIcon.h"

#include "ui/theme/Draw.h"
#include "ui/theme/Glyphs.h"
#include "ui/widgets/ChannelStrip.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
constexpr float kPi = 3.14159265f;

void lfoPath(DrawList& dl, float x, float y, float w, float h, std::uint32_t c, int cycles)
{
    constexpr int n = 48;
    float px = x, py = y + h / 2;
    for (int i = 1; i <= n; ++i)
    {
        const float t = static_cast<float>(i) / n * static_cast<float>(cycles);
        const float nx = x + w * static_cast<float>(i) / n, ny = y + h / 2 - std::sin(2 * kPi * t) * h / 2;
        glyph::line(dl, px, py, nx, ny, 2, c);
        px = nx;
        py = ny;
    }
}

void keyboardThumb(DrawList& dl, const tk::Palette& p, float x, float y, float w, float h)
{
    const float kw = w / 7;
    for (int i = 0; i < 7; ++i)
        dl.fill({x + static_cast<float>(i) * kw + 1, y, kw - 2, h}, theme::withOpacity(p.text1, 0.92f), 2);
    for (int i : {0, 1, 3, 4, 5})
        dl.fill({x + static_cast<float>(i + 1) * kw - kw * 0.3f, y, kw * 0.6f, h * 0.6f}, p.surface3, 2);
}

void midiPort(DrawList& dl, const tk::Palette& p, float cx, float cy, int n)
{
    ChannelStrip::arc(dl, cx, cy, 30, 0, 2 * kPi, 2, p.text2);
    for (int k = 0; k < 5; ++k)
    {
        const float a = kPi * (0.75f + static_cast<float>(k) * 0.375f);
        dl.fill({cx + 19 * std::cos(a) - 3, cy + 19 * std::sin(a) - 3, 6, 6}, p.text2, 3);
    }
    dl.fill({cx - 6, cy - 34, 12, 8}, p.surface1);
    dl.fill({cx - 6, cy - 31, 12, 2}, p.text2, 1);
    if (n > 0)
    {
        char t[12];
        std::snprintf(t, sizeof t, "%d", n);
        dl.text(cx, cy + 6, Align::Centre, TextStyleId::ValueS, p.text1, t);
    }
}
} // namespace

void ModuleCard::dashedRect(DrawList& dl, RectF r, std::uint32_t c)
{
    for (float x = r.x + 8; x < r.right() - 8; x += 12)
    {
        dl.fill({x, r.y, std::min(6.0f, r.right() - 8 - x), 1}, c);
        dl.fill({x, r.bottom() - 1, std::min(6.0f, r.right() - 8 - x), 1}, c);
    }
    for (float y = r.y + 8; y < r.bottom() - 8; y += 12)
    {
        dl.fill({r.x, y, 1, std::min(6.0f, r.bottom() - 8 - y)}, c);
        dl.fill({r.right() - 1, y, 1, std::min(6.0f, r.bottom() - 8 - y)}, c);
    }
}

void ModuleCard::cornerTriangle(DrawList& dl, RectF r, std::uint32_t c)
{
    for (int k = 0; k < 10; ++k) // right angle at (right − 4, bottom − 4), legs 10
        dl.fill({r.right() - 4 - static_cast<float>(k + 1), r.bottom() - 14 + static_cast<float>(k),
                 static_cast<float>(k + 1), 1},
                c);
}

void ModuleCard::connector(DrawList& dl, const UiContext& ctx, int slot)
{
    const auto r = rect(slot);
    dl.fill({r.right(), kY + 87, 16, 2}, ctx.palette.borderStrong, 1);
}

void ModuleCard::draw(DrawList& dl, const UiContext& ctx, const ModuleCardModel& m)
{
    const auto& p = ctx.palette;
    const auto r = rect(m.slot);
    if (m.state == ModuleCardModel::State::Empty)
    {
        dashedRect(dl, r, p.border);
        return;
    }
    const bool dis = m.state == ModuleCardModel::State::Disabled;
    dl.fill(r, dis ? p.surface1 : p.surface2, tk::radiusM);
    dl.text(r.x + kW / 2, r.y + 22, Align::Centre, TextStyleId::Label, dis ? p.text3 : p.text1, m.title);
    const RectF t{r.x + 16, r.y + 44, 152, 88};
    dl.fill(t, dis ? p.bg : p.surface1, tk::radiusS);
    if (! dis)
        switch (m.thumb)
        {
        case ModuleCardModel::Thumb::Wave:
            if (m.wave != nullptr && m.waveTo > m.waveFrom)
            {
                const float cy = t.y + 6 + 38;
                WaveformEditor::drawColumns(dl, *m.wave, m.waveFrom, m.waveTo, 70, t.x + 6, 2, 1.2f, cy, 38 * 0.92f,
                                            static_cast<std::int64_t>(m.waveFrom), static_cast<std::int64_t>(m.waveTo),
                                            m.colour);
                draw::hline(dl, t.x + 6, t.x + 146, cy, p.border);
                if (m.playhead >= 0)
                    dl.fill({t.x + 6 + 140 * std::min(1.0f, m.playhead) - 1, t.y + 4, 2, t.h - 8}, p.playhead);
            }
            break;
        case ModuleCardModel::Thumb::Envelope:
        {
            auto e = m.envelope;
            e.colour = m.colour;
            EnvelopeEditor::drawThumb(dl, ctx, t, e);
            break;
        }
        case ModuleCardModel::Thumb::Effect: // the effect's own icon (UI-261, 48 lpx), ⊘ for NONE
        {
            const float cx = t.x + 76, cy = t.y + 44;
            if (! m.effect.empty() && m.effect != "none") // hardware feedback: the card kept ⊘ whatever the slot held
            {
                EffectIcon::draw(dl, iconIdFor(m.effect), cx - 24, cy - 24, 48, m.bypassed ? p.text3 : p.text2);
                break;
            }
            ChannelStrip::arc(dl, cx, cy, 18, 0, 2 * kPi, 2, p.text2);
            glyph::line(dl, cx - 24 + 11.2f, cy - 24 + 36.8f, cx - 24 + 36.8f, cy - 24 + 11.2f, 2, p.text2);
            break;
        }
        case ModuleCardModel::Thumb::Lfo:
            lfoPath(dl, t.x + 10, t.y + 14, t.w - 20, t.h - 28, m.colour, 2);
            break;
        case ModuleCardModel::Thumb::Keys:
            keyboardThumb(dl, p, t.x + 12, t.y + 14, t.w - 24, t.h - 28);
            break;
        case ModuleCardModel::Thumb::Port:
            midiPort(dl, p, t.x + t.w / 2, t.y + t.h / 2, m.port);
            break;
        case ModuleCardModel::Thumb::Plugin:
        {
            const bool loaded = ! m.effect.empty() && m.effect != "none";
            const auto ink = loaded && ! m.bypassed ? m.colour : p.text2;
            const auto ring = loaded && ! m.bypassed ? p.text1 : p.text3;
            // Minimal synth module icon: 3 rotary rings over a mini step/patch bar.
            for (int k = 0; k < 3; ++k)
            {
                const float kx = t.x + 32.0f + static_cast<float>(k) * 44.0f;
                const float ky = t.y + 32.0f;
                ChannelStrip::arc(dl, kx, ky, 13.0f, 0, 2 * kPi, 2.0f, ring);
                const float ang = -kPi * 0.75f + static_cast<float>(k) * 0.65f;
                glyph::line(dl, kx, ky, kx + 9.0f * std::cos(ang), ky + 9.0f * std::sin(ang), 2.0f, ink);
            }
            for (int s = 0; s < 8; ++s)
            {
                const float bx = t.x + 18.0f + static_cast<float>(s) * 15.0f;
                const bool lit = loaded && (s % 2 == 0 || s == 5);
                dl.fill({bx, t.y + 62.0f, 10.0f, 12.0f}, lit ? ink : p.surface3, 2.0f);
            }
            break;
        }
        case ModuleCardModel::Thumb::None:
            break;
        }
    if (m.bypassed)
    {
        dl.fill(t, theme::withOpacity(p.bg, 0.6f), tk::radiusS);
        dl.fill({r.x + kW - 80, r.y + kH - 30, 68, 22}, p.warn, tk::radiusXs);
        dl.text(r.x + kW - 46, r.y + kH - 18, Align::Centre, TextStyleId::Caption, p.onAccent, "BYPASS");
    }
    if (m.bypassable)
        dashedRect(dl, {r.x - 4, r.y - 4, r.w + 8, r.h + 8}, p.warn);
    if (m.state == ModuleCardModel::State::Focused)
    {
        draw::focusRing(dl, p, r, tk::radiusM);
        cornerTriangle(dl, r, p.accent);
    }
    if (m.locked)
        dl.fill({r.x + kW - 14, r.y + 6, 8, 8}, p.mod, 2);
}
} // namespace ui
