#include "ui/widgets/ContextBar.h"

#include "ui/shell/Layout.h"
#include "ui/theme/Draw.h"
#include "ui/theme/Glyphs.h"

#include <cstdio>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
namespace L = layout;

// The touch regions and the painted faces share the same layout.
struct HeaderLayout
{
    RectF project, tempo, scene, position;
    float menuLeft, menuPitch;
    RectF menu(int i) const
    {
        return {menuLeft + menuPitch * i, project.y, L::menuButtonW, project.h};
    }
};

constexpr HeaderLayout normalHeader{L::slotA, L::slotB, L::slotC, L::slotD, L::slotE.x, L::menuPitch};
constexpr HeaderLayout classicHeader{{29, 58, 113, 78}, {152, 58, 236, 78}, {398, 58, 236, 78},
                                      {644, 58, 161, 78}, 806, 114};

bool in(const RectF& r, float x, float y) noexcept
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

std::uint32_t itemFill(const tk::Palette& p, bool selected, bool pressed)
{
    return pressed ? p.surface3 : (selected ? p.accentWeak : p.surface2);
}

void selectedBar(DrawList& dl, const tk::Palette& p, const RectF& r)
{
    dl.fill({r.x + tk::space[4], r.y, r.w - 2 * tk::space[4], 3}, p.accent, 1); // 2–3 lpx accent top bar
}

void splitItem(DrawList& dl, const tk::Palette& p, const RectF& r, std::string_view capA, std::string_view valA,
               std::string_view capB, std::string_view valB, bool selected, bool dim, int pressedHalf)
{
    draw::controlFace(dl, p, r, itemFill(p, selected, pressedHalf >= 0), tk::radiusM);
    if (selected)
        selectedBar(dl, p, r);
    draw::vline(dl, r.x + r.w / 2, r.y + 12, r.y + 60, p.borderStrong);
    const auto cap = dim ? p.text3 : p.text2, val = dim ? p.text3 : p.text1;
    for (int i = 0; i < 2; ++i)
    {
        const float cx = r.x + r.w / 4 + r.w / 2 * static_cast<float>(i);
        if (p.classic)
            dl.fill({cx - r.w / 4 + 6, r.y + 32, r.w / 2 - 12, 35}, p.bg, 2);
        dl.text(cx, r.y + 20, Align::Centre, p.classic ? TextStyleId::ValueS : TextStyleId::Caption, cap, i == 0 ? capA : capB);
        dl.text(cx, r.y + 52, Align::Centre, p.classic ? TextStyleId::ValueL : TextStyleId::Value, val, i == 0 ? valA : valB);
    }
}

// A caption badge (surface-3, radius 4, 22 tall) right-aligned to `right`; returns its left edge.
float badgeLeft(DrawList& dl, const tk::Palette& p, const TextMeasure& m, float right, float y, std::string_view text,
                std::uint32_t ink, bool glyph)
{
    const float w = glyph ? 28.0f : (m ? m(TextStyleId::Caption, text) : 8.0f * static_cast<float>(text.size())) + 16;
    const float x = right - w;
    dl.fill({x, y, w, 22}, p.surface3, tk::radiusXs);
    dl.text(x + w / 2, y + 12, Align::Centre, TextStyleId::Caption, ink, text);
    return x;
}
} // namespace

RectF ContextBar::bounds() const noexcept
{
    return L::contextBar;
}

void ContextBar::build(DrawList& dl, const UiContext& ctx) const
{
    const auto& p = ctx.palette;
    const auto& m = model_;
    const auto& slots = p.classic ? classicHeader : normalHeader;
    dl.fill(L::contextBar, p.classic ? p.bg : p.surface1);
    if (! p.classic)
        draw::hline(dl, 0, L::canvasW, L::contextY + L::contextH, p.border);

    // Slot A: PROJECT (document icon) or BACK (↩ arrow).
    draw::controlFace(dl, p, slots.project, m.pressed == SlotA ? p.surface3 : p.surface2, tk::radiusM);
    if (m.back)
        glyph::draw(dl, glyph::Kind::Back, slots.project.x + 38, slots.project.y + 16, tk::sizeIcon, p.text1);
    else
        glyph::draw(dl, glyph::Kind::Project, slots.project.x + 40, slots.project.y + 22, tk::sizeIcon, p.text1);

    splitItem(dl, p, slots.tempo, "BPM", m.bpm, "QUANTIZE", m.quantize, false, false,
              m.pressed == Bpm || m.pressed == Quantize ? 0 : -1);
    splitItem(dl, p, slots.scene, "SCENE", m.scene, "PATTERN", m.pattern, m.sceneSelected, m.sceneDimmed,
              m.pressed == Scene || m.pressed == Pattern ? 0 : -1);
    const float readoutX = p.classic ? slots.position.x + 5 : L::readoutX;
    dl.text(readoutX, p.classic ? 77 : L::captionY, Align::Left, p.classic ? TextStyleId::Body : TextStyleId::Caption,
            p.text2, "BAR:BEAT:STEP");
    dl.text(readoutX, p.classic ? 111 : L::valueY, Align::Left, TextStyleId::Value,
            m.playing ? p.play : p.text1, m.position);

    switch (m.kind)
    {
    case ContextBarKind::Menu:
    {
        static constexpr std::string_view captions[] = {"TRACKS", "MIXER", "ARRANGER", "UTILITY"};
        static constexpr glyph::Kind icons[] = {glyph::Kind::Tracks, glyph::Kind::Mixer, glyph::Kind::Arranger,
                                                glyph::Kind::Utility};
        for (int i = 0; i < 4; ++i)
        {
            const auto r = slots.menu(i);
            const bool sel = m.menuSelected == i;
            draw::controlFace(dl, p, r, itemFill(p, sel, m.pressed == Menu0 + i), tk::radiusM);
            if (p.classic)
            {
                const auto icon = i == 2 ? glyph::Kind::Score : (i == 3 ? glyph::Kind::Gear : icons[i]);
                glyph::draw(dl, icon, r.x + (r.w - 34) / 2, r.y + (r.h - 34) / 2, 34, sel ? p.accent : p.text1);
                continue;
            }
            if (sel)
                selectedBar(dl, p, r);
            glyph::draw(dl, icons[i], r.x + 40, r.y + 12, tk::sizeIcon, sel ? p.accent : p.text1);
            dl.text(r.x + r.w / 2, r.y + 56, Align::Centre, TextStyleId::Caption, sel ? p.accent : p.text2,
                    captions[i]);
        }
        break;
    }
    case ContextBarKind::TrackContext:
    {
        const auto& r = L::slotE;
        const auto colour = theme::trackColour(m.track);
        dl.fill(r, m.pressed == TrackBox ? p.surface3 : p.surface2, tk::radiusM);
        dl.stroke(r, colour, tk::borderTrackFrame, tk::radiusM);
        // TrackChip 40 × 40 at x + 16.
        dl.fill({r.x + 16, r.y + 16, 40, 40}, colour, tk::radiusXs);
        char number[4];
        std::snprintf(number, sizeof number, "%d", m.track);
        dl.text(r.x + 36, r.y + 37, Align::Centre, TextStyleId::Title, p.onAccent, number);
        float nameX = r.x + 72;
        if (m.missing) // ⚠ before the name (popups-and-feedback.md "Missing-sample warning")
        {
            glyph::draw(dl, glyph::Kind::Caution, nameX, r.y + 28, tk::sizeIconS, p.warn);
            nameX += 24;
        }
        dl.text(nameX, r.y + 36, Align::Left, TextStyleId::Title, m.trackName.empty() ? p.text3 : p.text1,
                m.trackName.empty() ? std::string_view("No sample") : m.trackName);
        float right = r.x + r.w - 16;
        if (! m.loopGlyph.empty())
            right = badgeLeft(dl, p, ctx.measure, right, r.y + 25, m.loopGlyph, colour, true) - 6;
        if (! m.stretchBadge.empty())
            right = badgeLeft(dl, p, ctx.measure, right, r.y + 25, m.stretchBadge, p.text2, false) - 6;
        if (! m.typeBadge.empty())
            badgeLeft(dl, p, ctx.measure, right, r.y + 25, m.typeBadge, p.text2, false);
        break;
    }
    case ContextBarKind::TrackTypeBypass:
    {
        const auto& t = L::trackTypeBox;
        draw::controlFace(dl, p, t, m.pressed == TrackType ? p.surface3 : p.surface2, tk::radiusM);
        dl.text(t.x + t.w / 2, (p.classic ? 77 : L::captionY), Align::Centre, TextStyleId::Caption, p.text2, "TRACK TYPE");
        if (p.classic)
            dl.fill({t.x + 6, t.y + 32, t.w - 12, 35}, p.bg, 2);
        dl.text(t.x + t.w / 2, (p.classic ? 111 : L::valueY), Align::Centre, TextStyleId::ValueS, p.text1, m.trackType);
        // BYPASS: a Toggle-styled item, warn LED bar when active (a bypassed track is a caution state).
        const auto& b = L::bypassBox;
        draw::controlFace(dl, p, b, m.pressed == Bypass ? p.surface3 : p.surface2, tk::radiusS);
        dl.fill({b.x + 16, b.y + 8, b.w - 32, 4}, m.bypass ? p.warn : p.borderStrong, 2);
        dl.text(b.x + b.w / 2, b.y + b.h / 2 + 4, Align::Centre, TextStyleId::Label, p.text1, "BYPASS");
        break;
    }
    case ContextBarKind::None:
        break;
    }
}

ContextBar::Item ContextBar::hit(float x, float y, bool classic) const noexcept
{
    const auto& slots = classic ? classicHeader : normalHeader;
    if (in(slots.project, x, y))
        return SlotA;
    if (in(slots.tempo, x, y))
        return x < slots.tempo.x + slots.tempo.w / 2 ? Bpm : Quantize;
    if (in(slots.scene, x, y))
        return x < slots.scene.x + slots.scene.w / 2 ? Scene : Pattern;
    if (in(slots.position, x, y))
        return Position;
    switch (model_.kind)
    {
    case ContextBarKind::Menu:
        for (int i = 0; i < 4; ++i)
            if (in(slots.menu(i), x, y))
                return static_cast<Item>(Menu0 + i);
        break;
    case ContextBarKind::TrackContext:
        if (in(L::slotE, x, y))
            return TrackBox;
        break;
    case ContextBarKind::TrackTypeBypass:
        if (in(L::trackTypeBox, x, y))
            return TrackType;
        if (in(L::bypassBox, x, y))
            return Bypass;
        break;
    case ContextBarKind::None:
        break;
    }
    return None;
}
} // namespace ui
