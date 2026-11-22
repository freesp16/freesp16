// OffscreenRenderer = the `soft` backend + a memory presenter + its own glyph atlas from the embedded fonts: renders a
// DrawList deterministically on any host, for snapshot, contract and benchmark tests. Damage tracking is on by default
// like on the device; setDamageTracking(false) redraws every frame. Optional cacheDir: the atlas goes through the disk
// cache (GlyphAtlas::loadOrBuild).
#pragma once

#include "ui/render/GlyphAtlas.h"
#include "ui/render/Renderer.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ui::render
{
class OffscreenRenderer
{
public:
    bool open(int width, int height, std::string& error, std::string_view cacheDir = {});
    Renderer& renderer() noexcept { return *renderer_; }
    const GlyphAtlas& atlas() const noexcept { return atlas_; }
    // Text metrics in canvas lpx (the shell's TextMeasure).
    std::function<float(TextStyleId, std::string_view)> measure() const;
    void setDamageTracking(bool on) { renderer_->setDamageTracking(on); }
    // Renders one frame and returns its pixels (0x00RRGGBB, row-major); empty when the renderer failed.
    std::vector<std::uint32_t> draw(const DrawList&, FrameTiming* timing = nullptr);

private:
    GlyphAtlas atlas_;
    std::unique_ptr<Renderer> renderer_;
};
} // namespace ui::render
