#include "ui/render/OffscreenRenderer.h"

namespace ui::render
{
bool OffscreenRenderer::open(int width, int height, std::string& error, std::string_view cacheDir)
{
    if (! atlas_.loadOrBuild(CanvasTransform::fit(width, height).scale, cacheDir, error))
        return false;
    renderer_ = makeCpuRenderer(makeSoftRasterizer(atlas_), makeMemoryPresenter(width, height), &atlas_);
    return renderer_ != nullptr;
}

std::function<float(TextStyleId, std::string_view)> OffscreenRenderer::measure() const
{
    return [this](TextStyleId s, std::string_view t) { return atlas_.measure(s, t) / atlas_.scale(); };
}

std::vector<std::uint32_t> OffscreenRenderer::draw(const DrawList& dl, FrameTiming* timing)
{
    std::vector<std::uint32_t> px;
    FrameTiming t;
    if (renderer_ && renderer_->render(dl, t))
        renderer_->readPixels(px);
    if (timing != nullptr)
        *timing = t;
    return px;
}
} // namespace ui::render
