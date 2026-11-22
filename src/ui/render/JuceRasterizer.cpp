// Backend "juce", CPU half: JUCE's software renderer (via JuceReplay) into a juce::Image, copied into the presenter's
// pixels. This is what JUCE's own X11 peer costs when it paints a Component tree in software (no GPU).
#include "ui/render/JuceReplay.h"
#include "ui/render/Renderer.h"

#include <cstring>
#include <optional>

namespace ui
{
namespace
{
class JuceRasterizer final : public CpuRasterizer
{
public:
    JuceRasterizer(const void* fontData, std::size_t fontSize) : replay_(fontData, fontSize) {}
    std::string_view name() const noexcept override { return "juce"; }

    void draw(const DrawList& list, std::uint32_t* pixels, int width, int height, int stride) override
    {
        drawClipped(list, pixels, width, height, stride, {0, 0, width, height});
    }

    // Damage: JUCE draws with the clip reduced to `clip` (cleared to black first, as a full frame starts), and only
    // those rows/columns are copied to the presenter.
    void drawClipped(const DrawList& list, std::uint32_t* pixels, int width, int height, int stride,
                     const IRect& clipIn) override
    {
        const IRect clip = intersect(clipIn, {0, 0, width, height});
        if (clip.empty())
            return;
        if (image_.getWidth() != width || image_.getHeight() != height)
            image_ = juce::Image(juce::Image::ARGB, width, height, true, juce::SoftwareImageType());
        {
            juce::Graphics g(image_);
            g.reduceClipRegion(clip.x0, clip.y0, clip.x1 - clip.x0, clip.y1 - clip.y0);
            g.fillAll(juce::Colours::black);
            replay_.draw(g, list, width, height);
        }
        const juce::Image::BitmapData data(image_, juce::Image::BitmapData::readOnly);
        for (int y = clip.y0; y < clip.y1; ++y) // ARGB is 0xAARRGGBB per uint32: same layout as 0x00RRGGBB
            std::memcpy(pixels + static_cast<std::ptrdiff_t>(y) * stride + clip.x0,
                        data.getLinePointer(y) + sizeof(std::uint32_t) * static_cast<std::size_t>(clip.x0),
                        sizeof(std::uint32_t) * static_cast<std::size_t>(clip.x1 - clip.x0));
    }

private:
    JuceReplay replay_;
    juce::Image image_;
};
} // namespace

std::unique_ptr<CpuRasterizer> makeJuceRasterizer(const void* fontData, std::size_t fontSize)
{
    return std::make_unique<JuceRasterizer>(fontData, fontSize);
}
} // namespace ui
