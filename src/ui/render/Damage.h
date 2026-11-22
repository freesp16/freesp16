// Damage tracking (UI redraw budget: an idle HOME ≤ 1 ms CPU per frame).
//
// Screens still append the whole frame to a DrawList every frame (immediate mode, widget contract); a backend redraws
// only what changed:
//   - FrameDiff compares the frame with the previous one command by command (hash + order-preserving alignment) and
//     reports the canvas bounds of every command that appeared, disappeared or changed. Unchanged commands in the same
//     relative order paint the same pixels, so everything outside those bounds is already correct on the surface.
//   - Producers may add explicit damage on top (DrawList::invalidate / invalidateAll, fed from Widget::dirty by the
//     shell): e.g. when something outside the command stream changed.
//   - DamageTracker merges the rects (at most kMaxRects, nearest-merge) in canvas lpx; backends convert them to
//     surface pixels (toSurface) and redraw with clipping (soft, juce) or the GL scissor (gl-fb, gl-x11).
// No allocation per frame after the first frames (vectors keep their capacity).
#pragma once

#include "ui/render/DrawList.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace ui
{
class GlyphAtlas;
struct CanvasTransform;

// Integer surface rectangle, half-open [x0, x1) × [y0, y1).
struct IRect
{
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool empty() const noexcept { return x1 <= x0 || y1 <= y0; }
    long area() const noexcept { return empty() ? 0 : static_cast<long>(x1 - x0) * (y1 - y0); }
    bool operator==(const IRect&) const = default;
};
IRect unite(const IRect& a, const IRect& b) noexcept;
IRect intersect(const IRect& a, const IRect& b) noexcept;

class DamageTracker
{
public:
    static constexpr int kMaxRects = 8;
    static constexpr float kCanvasW = 1280, kCanvasH = 768;

    void invalidate(RectF canvasRect) noexcept;
    void invalidateAll() noexcept;
    bool full() const noexcept { return full_; }
    bool empty() const noexcept { return ! full_ && count_ == 0; }
    // The merged damage (the whole canvas when full) and resets the tracker. The span stays valid until the next call.
    std::span<const RectF> take() noexcept;
    std::span<const RectF> rects() const noexcept { return {rects_.data(), static_cast<std::size_t>(count_)}; }

private:
    void add(RectF r) noexcept;
    std::array<RectF, kMaxRects> rects_{}, taken_{};
    int count_ = 0;
    bool full_ = false;
};

// Converts canvas damage to surface pixels (outward rounding plus an anti-aliasing margin), merges overlapping rects
// and returns them in `out` (cleared first). Returns the total area; the caller redraws everything when that is
// most of the surface.
long toSurface(std::span<const RectF> canvas, const CanvasTransform&, int width, int height, std::vector<IRect>& out);

// The per-command diff of consecutive frames.
class FrameDiff
{
public:
    // `atlas` measures text bounds (null: text damages its whole row band).
    explicit FrameDiff(const GlyphAtlas* atlas = nullptr) : atlas_(atlas) {}
    // Compares `now` with the frame of the previous call and adds the changed bounds (canvas lpx) to `out`. After
    // reset() (or the first call) the whole canvas is damaged.
    void diff(const DrawList& now, DamageTracker& out);
    void reset() noexcept { valid_ = false; }
    // Canvas bounds of one command, including stroke width and an anti-aliasing margin.
    RectF bounds(const DrawList& list, const DrawCmd& c) const noexcept;
    static std::uint64_t hash(const DrawList& list, const DrawCmd& c) noexcept;

private:
    const GlyphAtlas* atlas_;
    std::vector<std::uint64_t> prevHash_, nowHash_;
    std::vector<RectF> prevBounds_, nowBounds_;
    bool valid_ = false;
};

// What a backend redraws this frame: FrameDiff + the list's explicit damage, in surface pixels. Full when damage
// tracking is off, after forceFull() or a surface size change, or when the damage covers most of the surface.
class FrameDamage
{
public:
    enum class Kind : std::uint8_t
    {
        None,    // nothing changed: skip drawing (and presenting)
        Partial, // redraw rects()
        Full
    };
    explicit FrameDamage(const GlyphAtlas* atlas = nullptr) : diff_(atlas) {}
    Kind compute(const DrawList& list, int width, int height);
    std::span<const IRect> rects() const noexcept { return rects_; } // Partial: disjoint; Full: the whole surface
    long pixels() const noexcept { return pixels_; }
    void forceFull() noexcept { diff_.reset(); }
    void setEnabled(bool on) noexcept { enabled_ = on; }
    bool enabled() const noexcept { return enabled_; }
    static constexpr float kFullShare = 0.6f; // damage above this share of the surface redraws everything

private:
    FrameDiff diff_;
    DamageTracker tracker_;
    std::vector<IRect> rects_;
    long pixels_ = 0;
    int w_ = 0, h_ = 0;
    bool enabled_ = true;
};
} // namespace ui
