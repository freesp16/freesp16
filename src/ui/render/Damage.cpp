#include "ui/render/Damage.h"

#include "ui/render/GlyphAtlas.h"
#include "ui/render/Renderer.h"
#include "ui/theme/Theme.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ui
{
namespace
{
RectF uniteF(const RectF& a, const RectF& b) noexcept
{
    const float x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
    return {x0, y0, std::max(a.right(), b.right()) - x0, std::max(a.bottom(), b.bottom()) - y0};
}
float areaF(const RectF& r) noexcept
{
    return std::max(0.0f, r.w) * std::max(0.0f, r.h);
}
// Overlapping or closer than `slack` lpx: merging costs less than drawing the commands twice.
bool near(const RectF& a, const RectF& b, float slack) noexcept
{
    return a.x - slack < b.right() && b.x - slack < a.right() && a.y - slack < b.bottom() && b.y - slack < a.bottom();
}

inline std::uint64_t mix(std::uint64_t h, std::uint64_t v) noexcept
{
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}
inline std::uint32_t bits(float f) noexcept
{
    std::uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
} // namespace

IRect unite(const IRect& a, const IRect& b) noexcept
{
    if (a.empty())
        return b;
    if (b.empty())
        return a;
    return {std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}

IRect intersect(const IRect& a, const IRect& b) noexcept
{
    IRect r{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
    return r.empty() ? IRect{} : r;
}

void DamageTracker::invalidate(RectF r) noexcept
{
    if (full_ || r.w <= 0 || r.h <= 0)
        return;
    const float x0 = std::max(0.0f, r.x), y0 = std::max(0.0f, r.y);
    const float x1 = std::min(kCanvasW, r.right()), y1 = std::min(kCanvasH, r.bottom());
    if (x1 <= x0 || y1 <= y0)
        return;
    add({x0, y0, x1 - x0, y1 - y0});
}

void DamageTracker::add(RectF r) noexcept
{
    for (;;) // absorb every rect it touches, repeatedly (a merge can reach further rects)
    {
        bool merged = false;
        for (int i = 0; i < count_; ++i)
            if (near(rects_[static_cast<std::size_t>(i)], r, 4.0f))
            {
                r = uniteF(r, rects_[static_cast<std::size_t>(i)]);
                rects_[static_cast<std::size_t>(i)] = rects_[static_cast<std::size_t>(--count_)];
                merged = true;
                break;
            }
        if (! merged)
            break;
    }
    if (count_ < kMaxRects)
    {
        rects_[static_cast<std::size_t>(count_++)] = r;
        return;
    }
    // Full: merge into the rect whose union grows least.
    int best = 0;
    float bestGrowth = 1e30f;
    for (int i = 0; i < count_; ++i)
    {
        const auto& e = rects_[static_cast<std::size_t>(i)];
        const float growth = areaF(uniteF(e, r)) - areaF(e);
        if (growth < bestGrowth)
            bestGrowth = growth, best = i;
    }
    const RectF u = uniteF(rects_[static_cast<std::size_t>(best)], r);
    rects_[static_cast<std::size_t>(best)] = rects_[static_cast<std::size_t>(--count_)];
    add(u);
}

void DamageTracker::invalidateAll() noexcept
{
    full_ = true;
    count_ = 0;
}

std::span<const RectF> DamageTracker::take() noexcept
{
    std::size_t n = 0;
    if (full_)
        taken_[n++] = {0, 0, kCanvasW, kCanvasH};
    else
        for (int i = 0; i < count_; ++i)
            taken_[n++] = rects_[static_cast<std::size_t>(i)];
    full_ = false;
    count_ = 0;
    return {taken_.data(), n};
}

long toSurface(std::span<const RectF> canvas, const CanvasTransform& t, int width, int height, std::vector<IRect>& out)
{
    out.clear();
    const IRect surface{0, 0, width, height};
    for (const auto& r : canvas)
    {
        // 1 px anti-aliasing margin on top of outward rounding: rasterisers touch at most one pixel beyond a shape's
        // fractional edge.
        IRect s{static_cast<int>(std::floor(t.offsetX + r.x * t.scale)) - 1,
                static_cast<int>(std::floor(t.offsetY + r.y * t.scale)) - 1,
                static_cast<int>(std::ceil(t.offsetX + r.right() * t.scale)) + 1,
                static_cast<int>(std::ceil(t.offsetY + r.bottom() * t.scale)) + 1};
        s = intersect(s, surface);
        if (s.empty())
            continue;
        for (bool merged = true; merged;)
        {
            merged = false;
            for (std::size_t i = 0; i < out.size(); ++i)
                if (! intersect(out[i], s).empty())
                {
                    s = unite(s, out[i]);
                    out[i] = out.back();
                    out.pop_back();
                    merged = true;
                    break;
                }
        }
        out.push_back(s);
    }
    long area = 0;
    for (const auto& r : out)
        area += r.area();
    return area;
}

std::uint64_t FrameDiff::hash(const DrawList& list, const DrawCmd& c) noexcept
{
    std::uint64_t h = static_cast<std::uint64_t>(c.op) | static_cast<std::uint64_t>(c.align) << 8 |
                      static_cast<std::uint64_t>(c.style) << 16;
    h = mix(h, (static_cast<std::uint64_t>(bits(c.rect.x)) << 32) | bits(c.rect.y));
    h = mix(h, (static_cast<std::uint64_t>(bits(c.rect.w)) << 32) | bits(c.rect.h));
    h = mix(h, (static_cast<std::uint64_t>(bits(c.radius)) << 32) | bits(c.strokeWidth));
    h = mix(h, c.argb);
    if (c.op == DrawCmd::Op::Blit)
    {
        h = mix(h,
                (static_cast<std::uint64_t>(c.textOffset) << 32) | (static_cast<std::uint64_t>(c.srcW) << 16) | c.srcH);
        h = mix(h, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(c.pixels)));
    }
    else if (c.op == DrawCmd::Op::Text)
    {
        const auto s = list.textOf(c);
        std::size_t i = 0;
        for (; i + 8 <= s.size(); i += 8)
        {
            std::uint64_t w;
            std::memcpy(&w, s.data() + i, 8);
            h = mix(h, w);
        }
        std::uint64_t tail = s.size();
        for (; i < s.size(); ++i)
            tail = (tail << 8) | static_cast<unsigned char>(s[i]);
        h = mix(h, tail);
    }
    return h;
}

RectF FrameDiff::bounds(const DrawList& list, const DrawCmd& c) const noexcept
{
    constexpr float aa = 1.0f; // lpx; toSurface adds one more surface pixel
    switch (c.op)
    {
    case DrawCmd::Op::Fill:
    case DrawCmd::Op::Blit:
        return {c.rect.x - aa, c.rect.y - aa, c.rect.w + 2 * aa, c.rect.h + 2 * aa};
    case DrawCmd::Op::Stroke:
    {
        const float g = c.strokeWidth * 0.5f + aa;
        return {c.rect.x - g, c.rect.y - g, c.rect.w + 2 * g, c.rect.h + 2 * g};
    }
    case DrawCmd::Op::Text:
        break;
    }
    const float size = theme::textStyle(c.style).size;
    if (atlas_ == nullptr || atlas_->scale() <= 0)
        return {0, c.rect.y - size, DamageTracker::kCanvasW, 2 * size};
    const float w = atlas_->measure(c.style, list.textOf(c)) / atlas_->scale();
    const float x = c.align == Align::Left ? c.rect.x : c.align == Align::Centre ? c.rect.x - w * 0.5f : c.rect.x - w;
    const float pad = size * 0.25f + 2.0f; // side bearings, tracking, synthetic bold
    return {x - pad, c.rect.y - size, w + 2 * pad, 2 * size};
}

void FrameDiff::diff(const DrawList& now, DamageTracker& out)
{
    const auto cmds = now.commands();
    nowHash_.resize(cmds.size());
    nowBounds_.resize(cmds.size());
    for (std::size_t i = 0; i < cmds.size(); ++i)
    {
        nowHash_[i] = hash(now, cmds[i]);
        nowBounds_[i] = bounds(now, cmds[i]);
    }
    if (! valid_)
        out.invalidateAll();
    else
    {
        // Order-preserving greedy alignment: equal hashes advance both sides; otherwise look ahead (kLook commands)
        // for the nearest resynchronisation (an inserted or removed command) and damage what is skipped.
        constexpr std::size_t kLook = 16;
        const std::size_t n = prevHash_.size(), m = nowHash_.size();
        std::size_t i = 0, j = 0;
        while (i < n && j < m)
        {
            if (prevHash_[i] == nowHash_[j])
            {
                ++i, ++j;
                continue;
            }
            std::size_t skipOld = 0, skipNew = 0;
            bool found = false;
            for (std::size_t k = 1; k <= kLook && ! found; ++k)
            {
                if (i + k < n && prevHash_[i + k] == nowHash_[j])
                    skipOld = k, found = true;
                else if (j + k < m && prevHash_[i] == nowHash_[j + k])
                    skipNew = k, found = true;
            }
            if (! found) // changed in place
                skipOld = skipNew = 1;
            for (std::size_t k = 0; k < skipOld; ++k)
                out.invalidate(prevBounds_[i + k]);
            for (std::size_t k = 0; k < skipNew; ++k)
                out.invalidate(nowBounds_[j + k]);
            i += skipOld;
            j += skipNew;
        }
        for (; i < n; ++i)
            out.invalidate(prevBounds_[i]);
        for (; j < m; ++j)
            out.invalidate(nowBounds_[j]);
    }
    std::swap(prevHash_, nowHash_);
    std::swap(prevBounds_, nowBounds_);
    valid_ = true;
}

FrameDamage::Kind FrameDamage::compute(const DrawList& list, int width, int height)
{
    if (width != w_ || height != h_)
    {
        diff_.reset();
        w_ = width;
        h_ = height;
    }
    diff_.diff(list, tracker_); // always: keeps the previous frame current when tracking is switched back on
    if (list.invalidatedAll() || ! enabled_)
        tracker_.invalidateAll();
    for (const auto& r : list.invalidated())
        tracker_.invalidate(r);
    const bool full = tracker_.full();
    const auto canvas = tracker_.take();
    const long total = static_cast<long>(width) * height;
    if (! full)
    {
        pixels_ = toSurface(canvas, CanvasTransform::fit(width, height), width, height, rects_);
        if (rects_.empty())
            return Kind::None;
        if (static_cast<float>(pixels_) <= kFullShare * static_cast<float>(total))
            return Kind::Partial;
    }
    rects_.assign(1, IRect{0, 0, width, height});
    pixels_ = total;
    return Kind::Full;
}
} // namespace ui
