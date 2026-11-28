#include "ui/render/JuceReplay.h"

#include "ui/render/Renderer.h"
#include "ui/render/fonts/Fonts.h"
#include "ui/theme/Theme.h"

namespace ui
{
namespace
{
juce::Font makeFont(const juce::Typeface::Ptr& typeface, TextStyleId id)
{
    const auto& st = theme::textStyle(id);
    // FontOptions(typeface): a default FontOptions carries a name/style, which withTypeface() asserts against.
    auto options = typeface != nullptr ? juce::FontOptions(typeface) : juce::FontOptions();
    return juce::Font(options.withPointHeight(st.size).withKerningFactor(st.tracking).withFallbackEnabled(false));
}
} // namespace

JuceReplay::JuceReplay(const void* fontData, std::size_t fontSize)
{
    juce::Typeface::Ptr single;
    if (fontData != nullptr)
        single = juce::Typeface::createSystemTypefaceFor(fontData, fontSize);
    std::array<juce::Typeface::Ptr, static_cast<std::size_t>(fonts::Face::Count)> faces{};
    for (std::size_t s = 0; s < typefaces_.size(); ++s)
    {
        const auto id = static_cast<TextStyleId>(s);
        if (fontData != nullptr)
            typefaces_[s] = single;
        else
        {
            const auto f = fonts::faceForWeight(theme::textStyle(id).weight);
            auto& tf = faces[static_cast<std::size_t>(f)];
            if (tf == nullptr)
            {
                const auto b = fonts::blob(f);
                tf = juce::Typeface::createSystemTypefaceFor(b.data, b.size);
            }
            typefaces_[s] = tf;
        }
        fonts_.push_back(makeFont(typefaces_[s], id));
        juce::GlyphArrangement ga; // half the ink height of "x": the baseline below the centre line (GlyphAtlas)
        ga.addLineOfText(fonts_.back(), "x", 0.0f, 0.0f);
        juce::Path outline;
        ga.createPath(outline);
        xHalf_.push_back(outline.isEmpty() ? fonts_.back().getAscent() * 0.25f : -outline.getBounds().getY() * 0.5f);
    }
}

void JuceReplay::draw(juce::Graphics& g, const DrawList& list, int width, int height)
{
    const auto t = CanvasTransform::fit(width, height);
    g.saveState();
    g.addTransform(juce::AffineTransform::scale(t.scale).translated(t.offsetX, t.offsetY));
    for (const auto& c : list.commands())
    {
        const juce::Rectangle<float> r(c.rect.x, c.rect.y, c.rect.w, c.rect.h);
        g.setColour(juce::Colour(c.argb));
        switch (c.op)
        {
        case DrawCmd::Op::Fill:
            if (c.radius > 0)
                g.fillRoundedRectangle(r, c.radius);
            else
                g.fillRect(r);
            break;
        case DrawCmd::Op::Stroke:
            g.drawRoundedRectangle(r, c.radius, c.strokeWidth);
            break;
        case DrawCmd::Op::Text:
        {
            const auto& font = fonts_[static_cast<std::size_t>(c.style)];
            g.setFont(font);
            const auto sv = list.textOf(c);
            const auto text = juce::String::fromUTF8(sv.data(), static_cast<int>(sv.size()));
            const float baseline = c.rect.y + xHalf_[static_cast<std::size_t>(c.style)]; // CSS "middle"
            // Fractional anchor and baseline (drawSingleLineText rounds both to whole canvas units).
            juce::GlyphArrangement ga;
            ga.addLineOfText(font, text, 0.0f, baseline);
            const float w = ga.getNumGlyphs() > 0 ? ga.getBoundingBox(0, -1, true).getWidth() : 0.0f;
            const float x = c.align == Align::Left     ? c.rect.x
                            : c.align == Align::Centre ? c.rect.x - w * 0.5f
                                                       : c.rect.x - w;
            ga.moveRangeOfGlyphs(0, -1, x, 0.0f);
            ga.draw(g);
            break;
        }
        case DrawCmd::Op::Blit:
        {
            if (c.pixels == nullptr || c.srcW == 0 || c.srcH == 0)
                break;
            juce::Image img(juce::Image::ARGB, c.srcW, c.srcH, false);
            {
                juce::Image::BitmapData data(img, juce::Image::BitmapData::writeOnly);
                for (int y = 0; y < c.srcH; ++y)
                {
                    const auto* src = c.pixels + static_cast<std::ptrdiff_t>(y) * c.srcStride;
                    auto* dst = reinterpret_cast<juce::PixelARGB*>(data.getLinePointer(y));
                    for (int x = 0; x < c.srcW; ++x)
                    {
                        const std::uint32_t p = src[x];
                        dst[x].setARGB(static_cast<std::uint8_t>(p >> 24), static_cast<std::uint8_t>(p >> 16),
                                       static_cast<std::uint8_t>(p >> 8), static_cast<std::uint8_t>(p));
                        dst[x].premultiply();
                    }
                }
            }
            const float alpha = static_cast<float>(c.argb >> 24) / 255.0f;
            if (alpha < 1.0f)
                g.setOpacity(alpha);
            g.drawImage(img, r);
            break;
        }
        }
    }
    g.restoreState();
}
} // namespace ui
