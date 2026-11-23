// Custom software rasteriser: the DrawList -> 0x00RRGGBB pixels. Scanline spans with analytic edge coverage
// (fractional edges are common: the canvas scale on the 800 x 480 panel is 0.625), opaque spans as plain stores,
// alpha blending two channels per multiply, text as alpha blits from the GlyphAtlas. No allocation per frame.
// drawClipped() limits every span and blit to a clip rect (damage tracking): a pixel inside the clip gets exactly the
// value a full draw gives it, so partial redraws are bit-identical (tests/unit/render/test_damage.cpp). The blend loops
// are the NEON kernels of ui/render/simd/Blend.h.
#include "ui/render/GlyphAtlas.h"
#include "ui/render/Renderer.h"
#include "ui/render/simd/Blend.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace ui
{
namespace
{
using simd::blendPixel;

class SoftRasterizer final : public CpuRasterizer
{
public:
    explicit SoftRasterizer(const GlyphAtlas& atlas) : atlas_(atlas) {}
    std::string_view name() const noexcept override { return "soft"; }

    void draw(const DrawList& list, std::uint32_t* pixels, int width, int height, int stride) override
    {
        drawClipped(list, pixels, width, height, stride, {0, 0, width, height});
    }

    void drawClipped(const DrawList& list, std::uint32_t* pixels, int width, int height, int stride,
                     const IRect& clip) override
    {
        px_ = pixels;
        w_ = width;
        h_ = height;
        stride_ = stride;
        t_ = CanvasTransform::fit(width, height);
        clip_ = intersect(clip, {0, 0, width, height});
        if (clip_.empty())
            return;
        const auto cmds = list.commands();
        // Frames start with an opaque full-canvas fill (the screen background); otherwise clear the clip first so a
        // frame never depends on the previous one (letterbox and uncovered pixels are black).
        const bool opaqueBase = ! cmds.empty() && cmds[0].op == DrawCmd::Op::Fill && cmds[0].radius <= 0 &&
                                (cmds[0].argb >> 24) == 255 && cmds[0].rect.x <= 0 && cmds[0].rect.y <= 0 &&
                                cmds[0].rect.right() >= 1280 && cmds[0].rect.bottom() >= 768;
        if (! opaqueBase || t_.offsetX > 0 || t_.offsetY > 0)
            for (int y = clip_.y0; y < clip_.y1; ++y)
                std::fill(px_ + static_cast<std::ptrdiff_t>(y) * stride_ + clip_.x0,
                          px_ + static_cast<std::ptrdiff_t>(y) * stride_ + clip_.x1, 0u);
        const float cy0 = (static_cast<float>(clip_.y0) - t_.offsetY) / t_.scale - 1;
        const float cy1 = (static_cast<float>(clip_.y1) - t_.offsetY) / t_.scale + 1;
        const float cx0 = (static_cast<float>(clip_.x0) - t_.offsetX) / t_.scale - 1;
        const float cx1 = (static_cast<float>(clip_.x1) - t_.offsetX) / t_.scale + 1;
        for (const auto& c : cmds)
        {
            // Cull in canvas lpx (generous margins: culling only skips work, the clip decides the pixels).
            if (c.op == DrawCmd::Op::Text)
            {
                const float half = atlas_.metrics(c.style).sizePx / t_.scale + 2;
                if (c.rect.y + half < cy0 || c.rect.y - half > cy1)
                    continue;
            }
            else
            {
                const float g = c.op == DrawCmd::Op::Stroke ? c.strokeWidth : 0.0f;
                if (c.rect.y - g > cy1 || c.rect.bottom() + g < cy0 || c.rect.x - g > cx1 || c.rect.right() + g < cx0)
                    continue;
            }
            switch (c.op)
            {
            case DrawCmd::Op::Fill:
                fill(c.rect, c.radius, c.argb);
                break;
            case DrawCmd::Op::Stroke:
                stroke(c.rect, c.radius, c.strokeWidth, c.argb);
                break;
            case DrawCmd::Op::Text:
                text(c, list.textOf(c));
                break;
            case DrawCmd::Op::Blit:
                blit(c);
                break;
            }
        }
    }

private:
    // One horizontal span on row y from xl to xr (surface pixels, fractional), vertical coverage cy (0..1).
    void span(int y, float xl, float xr, float cy, std::uint32_t argb) noexcept
    {
        if (y < clip_.y0 || y >= clip_.y1)
            return;
        xl = std::max(xl, static_cast<float>(clip_.x0));
        xr = std::min(xr, static_cast<float>(clip_.x1));
        if (xr <= xl || cy <= 0.0f)
            return;
        std::uint32_t* row = px_ + static_cast<std::ptrdiff_t>(y) * stride_;
        const std::uint32_t rgb = argb & 0x00FFFFFFu;
        const float alpha = static_cast<float>(argb >> 24) * (1.0f / 255.0f);
        auto put = [&](int x, float coverage)
        {
            const auto a = static_cast<std::uint32_t>(coverage * alpha * 256.0f + 0.5f);
            if (a >= 256)
                row[x] = rgb;
            else if (a > 0)
                row[x] = blendPixel(row[x], rgb, a);
        };
        const int il = static_cast<int>(xl), ir = static_cast<int>(std::ceil(xr)) - 1;
        if (il >= ir)
        {
            put(il, (xr - xl) * cy);
            return;
        }
        put(il, (static_cast<float>(il + 1) - xl) * cy);
        put(ir, (xr - static_cast<float>(ir)) * cy);
        const auto a = static_cast<std::uint32_t>(cy * alpha * 256.0f + 0.5f);
        if (a >= 256)
            std::fill(row + il + 1, row + ir, rgb);
        else if (a > 0 && ir > il + 1)
            simd::blendSpan(row + il + 1, ir - il - 1, rgb, a);
    }

    // Horizontal extent of a rounded rect at row centre yc (inset of the corner arc).
    static float cornerInset(float yc, float y0, float y1, float r) noexcept
    {
        if (r <= 0.0f)
            return 0.0f;
        float dy = 0.0f;
        if (yc < y0 + r)
            dy = y0 + r - yc;
        else if (yc > y1 - r)
            dy = yc - (y1 - r);
        if (dy <= 0.0f)
            return 0.0f;
        dy = std::min(dy, r);
        // max(0, …): with FMA contraction r * r - dy * dy can come out a hair below 0 when dy == r (sqrt → NaN).
        return r - std::sqrt(std::max(0.0f, r * r - dy * dy));
    }

    void fill(RectF rc, float radius, std::uint32_t argb) noexcept
    {
        const float x0 = t_.offsetX + rc.x * t_.scale, x1 = x0 + rc.w * t_.scale;
        const float y0 = t_.offsetY + rc.y * t_.scale, y1 = y0 + rc.h * t_.scale;
        const float r = std::min(radius * t_.scale, std::min(x1 - x0, y1 - y0) * 0.5f);
        const int ya = std::max(clip_.y0, static_cast<int>(y0)),
                  yb = std::min(clip_.y1, static_cast<int>(std::ceil(y1)));
        for (int y = ya; y < yb; ++y)
        {
            const float cy = std::min(static_cast<float>(y + 1), y1) - std::max(static_cast<float>(y), y0);
            const float yc = std::clamp(static_cast<float>(y) + 0.5f, y0, y1);
            const float inset = cornerInset(yc, y0, y1, r);
            span(y, x0 + inset, x1 - inset, cy, argb);
        }
    }

    void stroke(RectF rc, float radius, float width, std::uint32_t argb) noexcept
    {
        const float hw = width * t_.scale * 0.5f;
        const float x0 = t_.offsetX + rc.x * t_.scale, x1 = x0 + rc.w * t_.scale;
        const float y0 = t_.offsetY + rc.y * t_.scale, y1 = y0 + rc.h * t_.scale;
        const float ox0 = x0 - hw, ox1 = x1 + hw, oy0 = y0 - hw, oy1 = y1 + hw;
        const float ix0 = x0 + hw, ix1 = x1 - hw, iy0 = y0 + hw, iy1 = y1 - hw;
        const float r = radius * t_.scale, ro = r + hw, ri = std::max(0.0f, r - hw);
        const int ya = std::max(clip_.y0, static_cast<int>(oy0)),
                  yb = std::min(clip_.y1, static_cast<int>(std::ceil(oy1)));
        for (int y = ya; y < yb; ++y)
        {
            const float cy = std::min(static_cast<float>(y + 1), oy1) - std::max(static_cast<float>(y), oy0);
            const float yc = std::clamp(static_cast<float>(y) + 0.5f, oy0, oy1);
            const float oi = cornerInset(yc, oy0, oy1, ro);
            const float l = ox0 + oi, rr = ox1 - oi;
            if (yc <= iy0 || yc >= iy1 || ix1 <= ix0)
            {
                // Above/below the inner rect: the whole row is outline; partial rows at the inner edge are blended.
                const float cover = yc <= iy0   ? std::min(cy, std::max(0.0f, iy0 - static_cast<float>(y)))
                                    : yc >= iy1 ? std::min(cy, std::max(0.0f, static_cast<float>(y + 1) - iy1))
                                                : cy;
                span(y, l, rr, ix1 <= ix0 ? cy : std::max(cover, 0.0f), argb);
                continue;
            }
            const float ii = cornerInset(yc, iy0, iy1, ri);
            span(y, l, std::min(ix0 + ii, rr), cy, argb);
            span(y, std::max(ix1 - ii, l), rr, cy, argb);
        }
    }

    void text(const DrawCmd& c, std::string_view s) noexcept
    {
        const auto& m = atlas_.metrics(c.style);
        float pen = t_.offsetX + c.rect.x * t_.scale;
        if (c.align != Align::Left)
        {
            const float w = atlas_.measure(c.style, s);
            pen -= c.align == Align::Centre ? w * 0.5f : w;
        }
        const float baseline = t_.offsetY + c.rect.y * t_.scale + m.baselineBelowCentre();
        const std::uint32_t rgb = c.argb & 0x00FFFFFFu, alpha = c.argb >> 24;
        const std::uint8_t* atlasPx = atlas_.pixels();
        const int aw = atlas_.width();
        for (std::size_t i = 0; i < s.size();)
        {
            const auto* g = atlas_.find(c.style, nextCodepoint(s, i));
            if (g->w != 0)
            {
                const int gx = static_cast<int>(std::lround(pen + g->left));
                const int gy = static_cast<int>(std::lround(baseline + g->top));
                const int x0 = std::max(clip_.x0, gx), x1 = std::min(clip_.x1, gx + g->w);
                const int y0 = std::max(clip_.y0, gy), y1 = std::min(clip_.y1, gy + g->h);
                for (int y = y0; y < y1 && x1 > x0; ++y)
                {
                    const std::uint8_t* src =
                        atlasPx + static_cast<std::ptrdiff_t>(g->y + (y - gy)) * aw + g->x + (x0 - gx);
                    simd::maskBlit(px_ + static_cast<std::ptrdiff_t>(y) * stride_ + x0, src, x1 - x0, rgb, alpha);
                }
            }
            pen += g->advance;
        }
    }

    void blit(const DrawCmd& c) noexcept
    {
        if (c.pixels == nullptr || c.srcW == 0 || c.srcH == 0)
            return;
        const float fx0 = t_.offsetX + c.rect.x * t_.scale;
        const float fy0 = t_.offsetY + c.rect.y * t_.scale;
        const float fw = c.rect.w * t_.scale;
        const float fh = c.rect.h * t_.scale;
        if (fw <= 0.0f || fh <= 0.0f)
            return;
        const int ix0 = static_cast<int>(std::lround(fx0));
        const int iy0 = static_cast<int>(std::lround(fy0));
        const int iw = static_cast<int>(std::lround(fw));
        const int ih = static_cast<int>(std::lround(fh));
        const int x0 = std::max(clip_.x0, ix0);
        const int x1 = std::min(clip_.x1, ix0 + iw);
        const int y0 = std::max(clip_.y0, iy0);
        const int y1 = std::min(clip_.y1, iy0 + ih);
        if (x0 >= x1 || y0 >= y1)
            return;
        const std::uint32_t tintA = c.argb >> 24;
        const bool oneToOne = (iw == c.srcW && ih == c.srcH);
        const bool opaqueTint = (c.argb == 0xFFFFFFFFu);
        for (int y = y0; y < y1; ++y)
        {
            const int sy = oneToOne ? (y - iy0)
                                    : std::clamp(static_cast<int>(static_cast<float>(y - iy0) * c.srcH / ih), 0,
                                                 static_cast<int>(c.srcH) - 1);
            const std::uint32_t* srcRow = c.pixels + static_cast<std::ptrdiff_t>(sy) * c.srcStride;
            std::uint32_t* dstRow = px_ + static_cast<std::ptrdiff_t>(y) * stride_;
            if (oneToOne && opaqueTint)
            {
                const std::uint32_t* s = srcRow + (x0 - ix0);
                for (int x = x0; x < x1; ++x, ++s)
                {
                    const std::uint32_t sp = *s;
                    const std::uint32_t a = sp >> 24;
                    if (a == 255)
                        dstRow[x] = sp & 0x00FFFFFFu;
                    else if (a > 0)
                        dstRow[x] = blendPixel(dstRow[x], sp & 0x00FFFFFFu, a + (a >> 7));
                }
            }
            else
            {
                for (int x = x0; x < x1; ++x)
                {
                    const int sx = oneToOne ? (x - ix0)
                                            : std::clamp(static_cast<int>(static_cast<float>(x - ix0) * c.srcW / iw), 0,
                                                         static_cast<int>(c.srcW) - 1);
                    const std::uint32_t sp = srcRow[sx];
                    const std::uint32_t sa = ((sp >> 24) * tintA + 127) / 255;
                    if (sa == 0)
                        continue;
                    std::uint32_t rgb = sp & 0x00FFFFFFu;
                    if (! opaqueTint)
                    {
                        const std::uint32_t r = (((sp >> 16) & 0xFFu) * ((c.argb >> 16) & 0xFFu) + 127) / 255;
                        const std::uint32_t g = (((sp >> 8) & 0xFFu) * ((c.argb >> 8) & 0xFFu) + 127) / 255;
                        const std::uint32_t b = ((sp & 0xFFu) * (c.argb & 0xFFu) + 127) / 255;
                        rgb = (r << 16) | (g << 8) | b;
                    }
                    dstRow[x] = sa >= 255 ? rgb : blendPixel(dstRow[x], rgb, sa + (sa >> 7));
                }
            }
        }
    }

    const GlyphAtlas& atlas_;
    std::uint32_t* px_ = nullptr;
    int w_ = 0, h_ = 0, stride_ = 0;
    IRect clip_{};
    CanvasTransform t_;
};

class MemoryPresenter final : public PixelPresenter
{
public:
    MemoryPresenter(int w, int h) : w_(w), h_(h), px_(static_cast<std::size_t>(w) * static_cast<std::size_t>(h)) {}
    int width() const noexcept override { return w_; }
    int height() const noexcept override { return h_; }
    std::uint32_t* pixels() noexcept override { return px_.data(); }
    int stridePixels() const noexcept override { return w_; }
    bool present() override { return true; }
    std::string describe() const override { return "offscreen " + std::to_string(w_) + "x" + std::to_string(h_); }

private:
    int w_, h_;
    std::vector<std::uint32_t> px_;
};

double nowMs() noexcept
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// A CPU rasteriser + a presenter, with damage tracking: only the changed rects are rasterised (clipped) and handed to
// the presenter; a frame without changes costs the diff only (no raster, no present).
class CpuRenderer final : public Renderer
{
public:
    CpuRenderer(std::unique_ptr<CpuRasterizer> r, std::unique_ptr<PixelPresenter> p, const GlyphAtlas* atlas)
        : raster_(std::move(r)), presenter_(std::move(p)), name_(raster_->name()), damage_(atlas)
    {
        rects_.reserve(DamageTracker::kMaxRects);
    }
    std::string_view name() const noexcept override { return name_; }
    int width() const noexcept override { return presenter_->width(); }
    int height() const noexcept override { return presenter_->height(); }
    void setDamageTracking(bool on) override { damage_.setEnabled(on); }
    void invalidate() override { damage_.forceFull(); }

    bool render(const DrawList& list, FrameTiming& t) override
    {
        const double a = nowMs();
        const int w = presenter_->width(), h = presenter_->height(), stride = presenter_->stridePixels();
        const auto kind = damage_.compute(list, w, h);
        t.damagedPixels = damage_.pixels();
        if (kind == FrameDamage::Kind::None)
        {
            t.rasterMs = nowMs() - a;
            t.presentMs = 0;
            return true;
        }
        if (kind == FrameDamage::Kind::Full)
            raster_->draw(list, presenter_->pixels(), w, h, stride);
        else
            for (const auto& r : damage_.rects())
                raster_->drawClipped(list, presenter_->pixels(), w, h, stride, r);
        const double b = nowMs();
        const bool ok =
            kind == FrameDamage::Kind::Full ? presenter_->present() : presenter_->presentRects(damage_.rects());
        t.rasterMs = b - a;
        t.presentMs = nowMs() - b;
        if (! ok)
            damage_.forceFull();
        return ok;
    }
    bool readPixels(std::vector<std::uint32_t>& out) override
    {
        const int w = width(), h = height(), stride = presenter_->stridePixels();
        out.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
        for (int y = 0; y < h; ++y)
            std::memcpy(out.data() + static_cast<std::ptrdiff_t>(y) * w,
                        presenter_->pixels() + static_cast<std::ptrdiff_t>(y) * stride,
                        sizeof(std::uint32_t) * static_cast<std::size_t>(w));
        return true;
    }
    std::string describe() const override
    {
        return std::string(name_) + " -> " + presenter_->describe() +
               (damage_.enabled() ? ", damage tracking" : ", full redraw");
    }

private:
    std::unique_ptr<CpuRasterizer> raster_;
    std::unique_ptr<PixelPresenter> presenter_;
    std::string name_;
    FrameDamage damage_;
    std::vector<IRect> rects_;
};
} // namespace

std::unique_ptr<CpuRasterizer> makeSoftRasterizer(const GlyphAtlas& atlas)
{
    return std::make_unique<SoftRasterizer>(atlas);
}
std::unique_ptr<PixelPresenter> makeMemoryPresenter(int w, int h)
{
    return std::make_unique<MemoryPresenter>(w, h);
}
std::unique_ptr<Renderer> makeCpuRenderer(std::unique_ptr<CpuRasterizer> r, std::unique_ptr<PixelPresenter> p,
                                          const GlyphAtlas* atlas)
{
    return std::make_unique<CpuRenderer>(std::move(r), std::move(p), atlas);
}
} // namespace ui
