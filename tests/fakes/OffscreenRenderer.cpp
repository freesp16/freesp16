#include "tests/fakes/OffscreenRenderer.h"

namespace fakes
{
bool OffscreenRenderer::open(int width, int height, std::string& error)
{
    if (! atlas_.build(ui::CanvasTransform::fit(width, height).scale, error)) // the embedded Inter set
        return false;
    renderer_ = ui::makeCpuRenderer(ui::makeSoftRasterizer(atlas_), ui::makeMemoryPresenter(width, height), &atlas_);
    return renderer_ != nullptr;
}

ui::TextMeasure OffscreenRenderer::measure() const
{
    return [this](ui::TextStyleId s, std::string_view t) { return atlas_.measure(s, t) / atlas_.scale(); };
}

std::vector<std::uint32_t> OffscreenRenderer::draw(const ui::DrawList& dl)
{
    std::vector<std::uint32_t> px;
    ui::FrameTiming t;
    if (renderer_ && renderer_->render(dl, t))
        renderer_->readPixels(px);
    return px;
}
} // namespace fakes
