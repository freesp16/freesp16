#include "ui/widgets/TrackTile.h"

#include "ui/shell/UiState.h"
#include "ui/theme/Draw.h"
#include "ui/theme/Glyphs.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;

float textW(const UiContext& ctx, TextStyleId s, std::string_view t)
{
    return ctx.measure ? ctx.measure(s, t) : 8.0f * static_cast<float>(t.size());
}

// A header badge 22 tall at y + 13 (mock.py badge()); w = 0 sizes it to the text + 16. Returns the next x.
float badge(DrawList& dl, const UiContext& ctx, float x, float y, std::string_view text, std::uint32_t fill,
            std::uint32_t ink, float w)
{
    if (w <= 0)
        w = textW(ctx, TextStyleId::Caption, text) + 16;
    dl.fill({x, y + 13, w, 22}, fill, tk::radiusXs);
    dl.text(x + w / 2, y + 25, Align::Centre, TextStyleId::Caption, ink, text);
    return x + w + 6;
}
void classicHeader(DrawList& dl, const UiContext& ctx, RectF r, const TrackTileModel& m,
                   const TrackSummary& s, std::uint32_t colour, std::string_view number)
{
    const auto& p = ctx.palette;
    const auto strip = s.muted ? theme::withOpacity(colour, 0.50f) : colour;
    float x = r.x + 12;
    auto segment = [&](std::string_view label, float width, std::uint32_t fill)
    {
        dl.fill({x, r.y + 14, width, 28}, fill, 1);
        dl.text(x + width / 2, r.y + 28, Align::Centre, TextStyleId::ValueS, p.onAccent, label);
        x += width + 2;
    };
    segment(number, 30, strip);
    if (m.variant == TrackTileModel::Variant::Setting)
    {
        segment(m.velBadge, 67, p.text2);
        segment(m.chokeBadge, 81, p.text2);
        segment(m.colourBadge, 50, strip);
    }
    else
    {
        segment(s.typeBadge, 69, strip);
        if (s.typeBadge == "SMPL")
        {
            segment(s.stretchBadge, 69, p.text2);
            segment(s.loopGlyph, 65, strip);
        }
        if (s.missing)
            glyph::draw(dl, glyph::Kind::Caution, r.right() - 31, r.bottom() - 30, 18, p.warn);
        const int level = TrackTile::levelSegments(m.levelDb);
        for (int i = 0; i < level; ++i)
            dl.fill({r.x + 12 + 13.0f * i, r.bottom() - 7, 11, 3}, strip);
    }
}
} // namespace

int TrackTile::levelSegments(float db) noexcept
{
    return db <= -48 ? 0 : std::min(5, 1 + static_cast<int>((db + 48.0f) / 10.0f));
}

std::string_view TrackTile::middleTruncate(std::string_view name, float maxW, TextStyleId style,
                                           const TextMeasure& measure, char* out, std::size_t n)
{
    if (! measure || name.size() + 4 > n || measure(style, name) <= maxW)
        return name;
    // Keep head and tail halves, drop characters from the middle until it fits (UTF-8 safe on code point bounds).
    std::size_t keep = name.size();
    while (keep > 2)
    {
        --keep;
        std::size_t head = (keep + 1) / 2, tail = keep - head;
        while (head > 0 && (static_cast<unsigned char>(name[head]) & 0xC0) == 0x80)
            --head;
        std::size_t from = name.size() - tail;
        while (from < name.size() && (static_cast<unsigned char>(name[from]) & 0xC0) == 0x80)
            ++from;
        std::memcpy(out, name.data(), head);
        std::memcpy(out + head, "\xE2\x80\xA6", 3);
        std::memcpy(out + head + 3, name.data() + from, name.size() - from);
        const std::string_view v{out, head + 3 + name.size() - from};
        if (measure(style, v) <= maxW)
            return v;
    }
    return name.substr(0, 1);
}

void TrackTile::draw(DrawList& dl, const UiContext& ctx, RectF b, const TrackTileModel& m)
{
    const auto& p = ctx.palette;
    static const TrackSummary kEmpty{};
    const auto& s = m.summary != nullptr ? *m.summary : kEmpty;
    const int colourIndex = m.colour > 0 ? m.colour : m.track;
    const auto colour = theme::trackColour(p, colourIndex);
    const bool setting = m.variant == TrackTileModel::Variant::Setting;

    draw::controlFace(dl, p, b, s.muted ? theme::withOpacity(p.surface2, 0.4f) : p.surface2, tk::radiusM);
    if (m.flash > 0) // hit: fill flashes to the track colour at 30 % and decays (motion.pad-flash)
        dl.fill(b, theme::withOpacity(colour, 0.3f * std::min(1.0f, m.flash)), p.classic ? 2 : tk::radiusM);
    if (s.soloed)
        dl.fill({b.x + 8, b.y, b.w - 16, 2}, p.warn, 1);

    char number[4];
    std::snprintf(number, sizeof number, "%d", m.track);
    if (p.classic)
        classicHeader(dl, ctx, b, m, s, colour, number);
    else
    {
        dl.fill({b.x + 12, b.y + 12, 32, 24}, s.hasContent ? colour : theme::trackColour(colourIndex, true), tk::radiusXs);
        dl.text(b.x + 28, b.y + 25, Align::Centre, TextStyleId::Label, s.hasContent ? p.onAccent : p.text1, number);

        if (setting)
        {
            const auto ink = s.hasContent ? p.text2 : p.text3;
            float x = badge(dl, ctx, b.x + 52, b.y, m.velBadge, p.surface1, ink, 72);
            x = badge(dl, ctx, x, b.y, m.chokeBadge, p.surface1, ink, 80);
            badge(dl, ctx, x, b.y, m.colourBadge, colour, p.onAccent, 48);
        }
        else if (! s.hasContent)
            badge(dl, ctx, b.x + 52, b.y, "SMPL", p.surface1, p.text3, 0);
        else
        {
            float x = badge(dl, ctx, b.x + 52, b.y, s.typeBadge, p.surface3, p.text1, 0);
            if (s.typeBadge == "SMPL")
            {
                x = badge(dl, ctx, x, b.y, s.stretchBadge, p.surface1, p.text2, 0);
                badge(dl, ctx, x, b.y, s.loopGlyph, p.surface1, colour, 32);
            }
            if (s.missing) // UID-504
                glyph::draw(dl, glyph::Kind::Caution, b.x + 206, b.y + 16, tk::sizeIconS, p.warn);
            const int segs = levelSegments(m.levelDb);
            for (int k = 0; k < 5; ++k)
                dl.fill({b.x + 232 + 8.0f * static_cast<float>(k), b.y + 19, 5, 10}, k < segs ? colour : p.surface3, 1);
        }
    }

    if (m.loading >= 0) // UI-410: a 4 lpx accent progress bar instead of the name
    {
        const float w = b.w - 32;
        dl.fill({b.x + 16, b.y + 68, w, 4}, p.surface3, 2);
        dl.fill({b.x + 16, b.y + 68, std::max(4.0f, w * std::min(1.0f, m.loading)), 4}, p.accent, 2);
    }
    else if (s.hasContent && ! s.name.empty())
    {
        // sampling.md "Take ready (unsaved)": an UNSAVED TAKE badge at the right of the name row until it is saved.
        float nameW = b.w - 32 - (p.classic && s.missing ? 28 : 0);
        if (s.unsavedTake)
        {
            constexpr std::string_view kUnsaved = "UNSAVED TAKE";
            const float w = textW(ctx, TextStyleId::Caption, kUnsaved) + 16;
            badge(dl, ctx, b.x + b.w - 16 - w, b.y + 45, kUnsaved, p.surface1, p.warn, w);
            nameW -= w + 8;
        }
        char buf[96];
        const auto name = middleTruncate(s.name, nameW, TextStyleId::BodyStrong, ctx.measure, buf, sizeof buf);
        dl.text(b.x + 16, b.y + 70, Align::Left, TextStyleId::BodyStrong, s.missing ? p.text3 : p.text1, name);
    }

    // One thin outline inside the tile (1 device px at x0.625, as on MIXER): the track colour when
    // selected, else the accent when the rotary's focus is on it — no second ring outside the tile.
    if (p.classic && (m.selected || m.focused))
        draw::focusRing(dl, p, b, 2);
    else if (m.selected || m.focused)
        dl.stroke({b.x + 0.8f, b.y + 0.8f, b.w - 1.6f, b.h - 1.6f}, m.selected ? colour : p.accent, 1.6f, tk::radiusM);
}
} // namespace ui
