// OffscreenRenderer = the `soft` backend + a memory presenter: renders a DrawList deterministically
// on any host, for snapshot and contract tests. Owns its glyph atlas, built at the surface's scale.
#pragma once

#include "ui/render/GlyphAtlas.h"
#include "ui/render/Renderer.h"
#include "ui/widgets/Widgets.h"

#include <memory>
#include <string>

namespace fakes
{
class OffscreenRenderer
{
public:
    // False + error when the atlas cannot be built.
    bool open(int width, int height, std::string& error);
    ui::Renderer& renderer() noexcept { return *renderer_; }
    const ui::GlyphAtlas& atlas() const noexcept { return atlas_; }
    ui::TextMeasure measure() const;
    // Renders and returns the frame's pixels (0x00RRGGBB, row-major).
    std::vector<std::uint32_t> draw(const ui::DrawList&);

private:
    ui::GlyphAtlas atlas_;
    std::unique_ptr<ui::Renderer> renderer_;
};
} // namespace fakes
