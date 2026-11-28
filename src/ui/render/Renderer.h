// Renderer backends. Extension point 5: GLES2 on fb, GLES2 on X11, software (custom rasteriser) and the JUCE-Graphics
// software replay used as the comparison spike.
//
// A backend draws a DrawList (canvas lpx) to its surface, scaled uniformly to fit (UID-101), and presents it.
// Registration: ui/render/RendererRegistry.cpp maps a name to a factory; the app tries the names of the
// "render" setting in order (auto = gl-fb, gl-x11, soft), each GL backend first probed in a child process.
//
// Damage (ui/render/Damage.h): every backend redraws only what changed since its previous frame — soft and
// juce with clipping and a partial present, gl-* with the scissor (buffer age / preserved swap) — and skips frames in
// which nothing changed (a vsynced backend then still paces to 60 Hz). setDamageTracking(false) redraws every frame.
#pragma once

#include "ui/render/Damage.h"
#include "ui/render/DrawList.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ui
{
class GlyphAtlas;

struct FrameTiming
{
    double rasterMs = 0;     // CPU time to turn the DrawList into pixels or GL commands
    double presentMs = 0;    // swap / XPutImage until the frame is handed over
    long damagedPixels = -1; // surface pixels redrawn this frame (0 = nothing changed, -1 = backend does not say)
};

class Renderer
{
public:
    virtual ~Renderer() = default;
    virtual std::string_view name() const noexcept = 0;
    virtual int width() const noexcept = 0;
    virtual int height() const noexcept = 0;
    // Draws and presents one frame. False when the surface is lost (the app then falls back to the next backend).
    virtual bool render(const DrawList& list, FrameTiming& timing) = 0;
    // The last frame as 0x00RRGGBB pixels, row-major, width() x height() (snapshot tests, screenshots).
    virtual bool readPixels(std::vector<std::uint32_t>& out) = 0;
    // One line for the log: driver, surface, pixel format.
    virtual std::string describe() const = 0;
    // Damage tracking on (default) or off (every frame redrawn in full: comparisons, benchmarks).
    virtual void setDamageTracking(bool on) { (void)on; }
    // The next frame is redrawn in full (surface contents lost, e.g. an expose).
    virtual void invalidate() {}
};

// CPU side of the software backends: turn a DrawList into 0x00RRGGBB pixels.
class CpuRasterizer
{
public:
    virtual ~CpuRasterizer() = default;
    virtual std::string_view name() const noexcept = 0;
    virtual void draw(const DrawList& list, std::uint32_t* pixels, int width, int height, int stridePixels) = 0;
    // Redraws only `clip` (surface pixels): every pixel inside it ends exactly as a full draw() would leave it, pixels
    // outside are not touched. The default draws everything (correct, not cheap).
    virtual void drawClipped(const DrawList& list, std::uint32_t* pixels, int width, int height, int stridePixels,
                             const IRect& clip)
    {
        (void)clip;
        draw(list, pixels, width, height, stridePixels);
    }
};

// Where software backends' pixels go: an offscreen buffer (tests, host snapshots) or an X11 window (device).
class PixelPresenter
{
public:
    virtual ~PixelPresenter() = default;
    virtual int width() const noexcept = 0;
    virtual int height() const noexcept = 0;
    virtual std::uint32_t* pixels() noexcept = 0;
    virtual int stridePixels() const noexcept = 0;
    virtual bool present() = 0;
    // Hands over only `rects` (the rest of the surface is unchanged since the previous present). Default: present().
    virtual bool presentRects(std::span<const IRect> rects)
    {
        (void)rects;
        return present();
    }
    virtual std::string describe() const = 0;
};

std::unique_ptr<CpuRasterizer> makeSoftRasterizer(const GlyphAtlas& atlas);
// fontData null: the embedded Inter faces in each style's weight (the same fonts as the glyph atlas).
std::unique_ptr<CpuRasterizer> makeJuceRasterizer(const void* fontData, std::size_t fontSize);
std::unique_ptr<PixelPresenter> makeMemoryPresenter(int width, int height);
// `atlas` (optional) measures text for tight damage bounds; without it changed text damages its whole row band.
std::unique_ptr<Renderer> makeCpuRenderer(std::unique_ptr<CpuRasterizer>, std::unique_ptr<PixelPresenter>,
                                          const GlyphAtlas* atlas = nullptr);
} // namespace ui
