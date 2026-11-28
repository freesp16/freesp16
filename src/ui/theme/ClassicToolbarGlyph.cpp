// Classic toolbar silhouettes. Rasterise each closed outline once at the current display scale.
#include "ui/theme/Glyphs.h"

#include <juce_graphics/juce_graphics.h>

#include <cmath>
#include <optional>

namespace ui::glyph
{
namespace
{
juce::Path note()
{
    juce::Path p;
    p.startNewSubPath(-0.55f, -8.55f);
    p.quadraticTo(-0.4f, -9.55f, 0.7f, -8.8f);
    p.cubicTo(1.7f, -6.6f, 3.2f, -5.6f, 4.9f, -4.2f);
    p.cubicTo(7.5f, -2.1f, 7.5f, 0.6f, 5.55f, 2.6f);
    p.quadraticTo(5.0f, 3.0f, 5.25f, 2.15f);
    p.cubicTo(6.15f, -0.4f, 5.25f, -1.9f, 3.65f, -3.0f);
    p.lineTo(1.15f, -4.65f);
    p.lineTo(1.15f, 5.15f);
    p.cubicTo(1.3f, 7.05f, -0.55f, 8.7f, -3.0f, 9.0f);
    p.cubicTo(-5.1f, 9.35f, -6.65f, 8.55f, -6.7f, 7.15f);
    p.cubicTo(-6.8f, 5.15f, -3.3f, 3.3f, -0.55f, 4.1f);
    p.closeSubPath();
    return p;
}

juce::Path gear()
{
    juce::Path rim;
    rim.startNewSubPath(-3.05f, -7.35f);
    for (int tooth = 0; tooth < 8; ++tooth)
    {
        const auto rotation = juce::AffineTransform::rotation(tooth * juce::MathConstants<float>::pi / 4);
        for (auto point :
             {juce::Point<float>{-2.0f, -7.65f}, {-1.60f, -10.0f}, {1.60f, -10.0f}, {2.0f, -7.65f}, {3.05f, -7.35f}})
            rim.lineTo(point.transformedBy(rotation));
    }
    rim.closeSubPath();
    auto p = rim.createPathWithRoundedCorners(0.6f);
    p.setUsingNonZeroWinding(false);
    p.addEllipse(-4.15f, -4.15f, 8.3f, 8.3f);
    return p;
}

// Emit disjoint runs of equal coverage. A curved edge gets blended once, including at tooth roots.
struct Ink
{
    DrawList& dl;
    float x, y, pixel;
    std::uint32_t colour;
    int row = 0;
    void setEdgeTableYPos(int py) { row = py; }
    void handleEdgeTableLine(int px, int width, int coverage) const
    {
        const auto alpha = ((colour >> 24) * static_cast<unsigned>(coverage) + 127u) / 255u;
        dl.fill({x + px * pixel, y + row * pixel, width * pixel, pixel}, (colour & 0x00ffffffu) | (alpha << 24));
    }
    void handleEdgeTableLineFull(int px, int width) const { handleEdgeTableLine(px, width, 255); }
    void handleEdgeTablePixel(int px, int coverage) const { handleEdgeTableLine(px, 1, coverage); }
    void handleEdgeTablePixelFull(int px) const { handleEdgeTableLineFull(px, 1); }
};

struct Mask
{
    int diameter = 0;
    std::optional<juce::EdgeTable> edges;
};
} // namespace

void drawClassicToolbarGlyph(DrawList& dl, Kind kind, float x, float y, float size, std::uint32_t colour)
{
    const auto& view = dl.viewport;
    const int diameter = std::max(1, static_cast<int>(std::lround(size * view.scale)));
    const float scale = static_cast<float>(diameter) / 21.0f;
    const float pixel = 1.0f / view.scale;
    const auto centre = [&](float pos, float offset)
    { return (std::round((pos + 0.5f * size) * view.scale + offset) - offset) * pixel; };
    const float cx = centre(x, view.offsetX), cy = centre(y, view.offsetY);
    if (kind == Kind::Score)
    {
        // Keep one clear pixel between the note and staff on the panel.
        constexpr float leftEnds[] = {-2.4f, -2.4f, -2.4f, -4.65f, -8.2f};
        constexpr float rightStarts[] = {4.4f, 7.6f, 8.25f, 6.2f, 3.0f};
        const auto staff = (colour & 0x00ffffffu) | (((colour >> 24) * 182u / 255u) << 24);
        const float unit = scale * pixel;
        for (int i = 0; i < 5; ++i)
        {
            const float top = cy + std::round((3 * i - 6) * scale) * pixel;
            dl.fill({cx - 13 * unit, top, (13 + leftEnds[i]) * unit, unit}, staff);
            dl.fill({cx + rightStarts[i] * unit, top, (13 - rightStarts[i]) * unit, unit}, staff);
        }
    }

    // Keep the raster bounds positive, then place the mask back about the button centre.
    const float origin = static_cast<float>((diameter + 1) / 2 + 1);
    static thread_local Mask scoreMask, gearMask;
    auto& mask = kind == Kind::Score ? scoreMask : gearMask;
    if (mask.diameter != diameter)
    {
        auto path = kind == Kind::Score ? note() : gear();
        path.applyTransform(juce::AffineTransform::scale(scale).translated(origin, origin));
        mask.edges.emplace(path.getBounds().getSmallestIntegerContainer().expanded(1), path, juce::AffineTransform{});
        mask.diameter = diameter;
    }
    Ink ink{dl, cx - origin * pixel, cy - origin * pixel, pixel, colour};
    mask.edges->iterate(ink);
}
} // namespace ui::glyph
