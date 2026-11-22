// A frame's drawing commands in canvas coordinates (lpx on the 1280 x 768 canvas, 03-layout-grid.md). Screens and
// widgets only ever append to a DrawList; every Renderer backend (GLES2 fb/x11, the custom software rasteriser, the
// JUCE Graphics replay) consumes the same list, so backends are interchangeable and comparable.
//
// Colours are 0xAARRGGBB with straight alpha (theme tokens). Text is UTF-8, positioned by its vertical centre line
// like the mockups' dominant-baseline="middle": the alphabetic baseline sits half the style's
// x-height below y. After reserve(), appending does not allocate.
//
// Damage (ui/render/Damage.h): a DrawList is always the whole frame; backends diff it against the previous frame and
// redraw only what changed. invalidate()/invalidateAll() add explicit damage on top (the shell forwards Widget::dirty
// rects, or forces a full redraw); clear() drops it with the commands.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace ui
{
struct RectF
{
    float x = 0, y = 0, w = 0, h = 0;
    float right() const noexcept { return x + w; }
    float bottom() const noexcept { return y + h; }
};

enum class Align : std::uint8_t
{
    Left,
    Centre,
    Right
};

// The token type styles (tokens.json font.style), in generated order.
enum class TextStyleId : std::uint8_t
{
    Caption,
    Label,
    Body,
    BodyStrong,
    Title,
    ValueS,
    Value,
    ValueL,
    Display,
    Count
};

struct DrawCmd
{
    enum class Op : std::uint8_t
    {
        Fill,   // rounded rectangle, radius may be 0
        Stroke, // rounded rectangle outline of strokeWidth, centred on the rect edge
        Text
    };
    Op op = Op::Fill;
    Align align = Align::Left;
    TextStyleId style = TextStyleId::Body;
    RectF rect{}; // Text: x = anchor x, y = centre line
    float radius = 0, strokeWidth = 0;
    std::uint32_t argb = 0;
    std::uint32_t textOffset = 0, textLength = 0;
};

class DrawList
{
public:
    void reserve(std::size_t commands, std::size_t textBytes)
    {
        cmds_.reserve(commands);
        text_.reserve(textBytes);
        damage_.reserve(32);
    }
    void clear() noexcept
    {
        cmds_.clear();
        text_.clear();
        damage_.clear();
        damageAll_ = false;
    }

    // Explicit damage in canvas lpx, redrawn even when the commands there did not change.
    void invalidate(RectF r)
    {
        if (r.w > 0 && r.h > 0)
            damage_.push_back(r);
    }
    void invalidateAll() noexcept { damageAll_ = true; }
    std::span<const RectF> invalidated() const noexcept { return damage_; }
    bool invalidatedAll() const noexcept { return damageAll_; }

    void fill(RectF r, std::uint32_t argb, float radius = 0)
    {
        if ((argb >> 24) != 0)
            cmds_.push_back({DrawCmd::Op::Fill, Align::Left, TextStyleId::Body, r, radius, 0, argb, 0, 0});
    }
    void stroke(RectF r, std::uint32_t argb, float width, float radius = 0)
    {
        if ((argb >> 24) != 0)
            cmds_.push_back({DrawCmd::Op::Stroke, Align::Left, TextStyleId::Body, r, radius, width, argb, 0, 0});
    }
    void text(float x, float yCentre, Align align, TextStyleId style, std::uint32_t argb, std::string_view utf8)
    {
        const auto offset = static_cast<std::uint32_t>(text_.size());
        text_.insert(text_.end(), utf8.begin(), utf8.end());
        cmds_.push_back({DrawCmd::Op::Text,
                         align,
                         style,
                         {x, yCentre, 0, 0},
                         0,
                         0,
                         argb,
                         offset,
                         static_cast<std::uint32_t>(utf8.size())});
    }

    std::span<const DrawCmd> commands() const noexcept { return cmds_; }
    std::string_view textOf(const DrawCmd& c) const noexcept { return {text_.data() + c.textOffset, c.textLength}; }

private:
    std::vector<DrawCmd> cmds_;
    std::vector<char> text_;
    std::vector<RectF> damage_;
    bool damageAll_ = false;
};

// Decodes one UTF-8 code point and advances i; invalid bytes decode as U+FFFD.
inline char32_t nextCodepoint(std::string_view s, std::size_t& i) noexcept
{
    const auto b = static_cast<unsigned char>(s[i++]);
    if (b < 0x80)
        return b;
    int extra = (b >= 0xF0) ? 3 : (b >= 0xE0) ? 2 : (b >= 0xC0) ? 1 : -1;
    if (extra < 0)
        return 0xFFFD;
    char32_t cp = b & (0x3F >> extra);
    while (extra-- > 0)
    {
        if (i >= s.size())
            return 0xFFFD;
        cp = (cp << 6) | (static_cast<unsigned char>(s[i++]) & 0x3F);
    }
    return cp;
}
} // namespace ui
