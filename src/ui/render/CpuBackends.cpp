// The CPU backends: `soft` (custom scanline rasteriser + glyph atlas, the guaranteed fallback and snapshot
// reference) and `juce` (JUCE software Graphics, the comparison spike). Both draw into the platform's presenter
// (RendererEnv::windowPresenter: an X11 window on imx6, memory on the host and in tests) with damage tracking.
// Fonts: `juce` uses the embedded Inter faces (the atlas's faces); env.fontData only overrides them for a single-font
// atlas.
#include "ui/render/GlyphAtlas.h"
#include "ui/render/RendererRegistry.h"

namespace ui
{
namespace
{
std::unique_ptr<Renderer> makeCpu(const RendererEnv& env, bool soft, std::string& error)
{
    if (soft && env.atlas == nullptr)
        return error = "soft needs the glyph atlas", nullptr;
    // juce: one given font only when the atlas was built from it; otherwise (and by default) the embedded Inter faces.
    const bool embedded = env.fontData == nullptr || (env.atlas != nullptr && env.atlas->embeddedFonts());
    if (! env.windowPresenter)
        return error = "no presenter on this platform", nullptr;
    auto presenter = env.windowPresenter(error);
    if (! presenter)
        return nullptr;
    auto raster = soft ? makeSoftRasterizer(*env.atlas)
                       : makeJuceRasterizer(embedded ? nullptr : env.fontData, embedded ? 0 : env.fontSize);
    return makeCpuRenderer(std::move(raster), std::move(presenter), env.atlas);
}

std::unique_ptr<Renderer> makeSoft(const RendererEnv& env, std::string& error)
{
    return makeCpu(env, true, error);
}
std::unique_ptr<Renderer> makeJuce(const RendererEnv& env, std::string& error)
{
    return makeCpu(env, false, error);
}
} // namespace
} // namespace ui

APP_REGISTER_RENDERER("soft", &ui::makeSoft, false)
APP_REGISTER_RENDERER("juce", &ui::makeJuce, false)
