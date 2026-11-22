#include "ui/widgets/RingView.h"

#include "ui/theme/Draw.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
using core::sampling::LiveState;
constexpr float kPlotX = 336, kPlotW = 840, kNowX = kPlotX + 630, kPitch = 3, kTop = 160, kBottom = 504;
constexpr float kWaveCy = 332, kWaveHalf = 140 * 0.92f; // the mockup's waveform(): h − 80, scale 0.92
constexpr float kRegionY = 188, kRegionH = 288;

// A dashed rectangle outline (6 on, 4 off) of `w` lpx: DrawList has no dash, so short fills.
void dashedRect(DrawList& dl, RectF r, float w, std::uint32_t c)
{
    for (float x = r.x; x < r.right(); x += 10)
    {
        const float len = std::min(6.0f, r.right() - x);
        dl.fill({x, r.y - w / 2, len, w}, c);
        dl.fill({x, r.bottom() - w / 2, len, w}, c);
    }
    for (float y = r.y; y < r.bottom(); y += 10)
    {
        const float len = std::min(6.0f, r.bottom() - y);
        dl.fill({r.x - w / 2, y, w, len}, c);
        dl.fill({r.right() - w / 2, y, w, len}, c);
    }
}
} // namespace

double RingView::framesPerColumn() const noexcept
{
    const double bpm = feed_.bpm > 1 ? feed_.bpm : 120.0;
    const double bar = 4.0 * 60.0 / bpm * feed_.sampleRate;
    return 8.0 * bar / kColumns;
}

float RingView::xOf(std::int64_t frame) const noexcept
{
    return kNowX + static_cast<float>(static_cast<double>(frame - now_) / framesPerColumn()) * kPitch;
}

void RingView::reset() noexcept
{
    for (auto& c : cache_)
        c = Column {};
    done_ = -1;
    built_ = 0;
}

const RingView::Column* RingView::find(std::int64_t index) const noexcept
{
    if (index < 0)
        return nullptr;
    const auto& c = cache_[static_cast<std::size_t>(index % kCache)];
    return c.index == index ? &c : nullptr;
}

float RingView::peakAt(std::int64_t frame) const noexcept
{
    const auto* c = find(static_cast<std::int64_t>(std::floor(static_cast<double>(frame) / fpc_)));
    return c != nullptr ? std::max(c->hi, -c->lo) : -1.0f;
}

void RingView::fold(std::int64_t from, std::int64_t to) noexcept
{
    const auto* ring = feed_.ring;
    while (from < to)
    {
        const int n = static_cast<int>(std::min<std::int64_t>(kChunk, to - from));
        if (! ring->read(from, n, l_.data(), r_.data()))
            return; // overwritten meanwhile (or not written): the next update starts again from the head
        int i = 0;
        while (i < n)
        {
            const std::int64_t f = from + i;
            const auto index = static_cast<std::int64_t>(std::floor(static_cast<double>(f) / fpc_));
            const auto next = static_cast<std::int64_t>(std::ceil(static_cast<double>(index + 1) * fpc_));
            const int end = static_cast<int>(std::min<std::int64_t>(n, std::max<std::int64_t>(i + 1, next - from)));
            auto& c = column(index);
            if (c.index != index)
                c = Column {index, 0, 0};
            float lo = c.lo, hi = c.hi;
            for (int k = i; k < end; ++k)
            {
                lo = std::min({lo, l_[static_cast<std::size_t>(k)], r_[static_cast<std::size_t>(k)]});
                hi = std::max({hi, l_[static_cast<std::size_t>(k)], r_[static_cast<std::size_t>(k)]});
            }
            c.lo = lo;
            c.hi = hi;
            ++built_;
            i = end;
        }
        from += n;
    }
}

void RingView::update() noexcept
{
    built_ = 0;
    const auto* ring = feed_.ring;
    if (ring == nullptr || ring->capacity() == 0)
        return;
    const double fpc = framesPerColumn();
    if (fpc != fpc_)
    {
        reset(); // a tempo change: the columns are rebuilt from the ring once
        fpc_ = fpc;
    }
    if (! feed_.playing && done_ >= 0)
        return; // stopped: the view is frozen (sampling.md "Transport stopped")
    now_ = ring->head();
    const auto window = static_cast<std::int64_t>(std::ceil(fpc_ * (kPast + 1)));
    std::int64_t from = std::max<std::int64_t>({done_, now_ - window, ring->oldest()});
    if (from < 0)
        from = 0;
    fold(from, now_);
    done_ = now_;
}

void RingView::build(DrawList& dl, const UiContext& ctx) const
{
    const auto& p = ctx.palette;
    dl.fill(kRect, p.surface1, tk::radiusM);
    if (feed_.ring == nullptr || fpc_ <= 0)
        return;
    const double bar = fpc_ * kColumns / 8.0;
    // Bar lines (play 35 %) at the transport's bar starts, labelled by their distance from NOW.
    const double barPos = feed_.ppq / 4.0, frac = barPos - std::floor(barPos);
    const auto lastBar = now_ - static_cast<std::int64_t>(std::llround(frac * bar));
    const auto lineInk = theme::withOpacity(p.play, 0.35f);
    char label[8];
    for (int b = -7; b <= 2; ++b)
    {
        const float x = xOf(lastBar + static_cast<std::int64_t>(std::llround(b * bar)));
        if (x < kPlotX - 0.5f || x > kPlotX + kPlotW + 0.5f)
            continue;
        dl.fill({x - 0.5f, kTop, 1, kBottom - kTop}, lineInk);
        const bool atNow = std::fabs(x - kNowX) < 1.0f;
        const float barPx = static_cast<float>(bar / fpc_) * kPitch, d = (x - kNowX) / barPx;
        const int rel = d < 0 ? -static_cast<int>(std::ceil(-d - 0.01f)) : static_cast<int>(std::ceil(d - 0.01f));
        if (atNow || x + 30 > kPlotX + kPlotW)
            continue;
        std::snprintf(label, sizeof label, "%+d", rel);
        dl.text(x + 6, 172, Align::Left, TextStyleId::Caption, p.text3, label);
    }
    dl.text(kNowX + 6, 172, Align::Left, TextStyleId::Caption, p.text1, "NOW");
    // The input up to NOW: text-2 columns, the take's own columns in the track colour while it records.
    draw::hline(dl, kPlotX, kNowX, kWaveCy, p.border);
    const auto nowIndex = static_cast<std::int64_t>(std::floor(static_cast<double>(now_) / fpc_));
    const bool taking = feed_.state == static_cast<int>(LiveState::Recording) && feed_.takeStart >= 0;
    const std::int64_t t0 = feed_.takeStart, t1 = feed_.takeStart + feed_.takeLength;
    for (std::int64_t k = nowIndex - kPast; k < nowIndex; ++k)
    {
        const auto* c = find(k);
        if (c == nullptr)
            continue;
        const float x = xOf(static_cast<std::int64_t>((static_cast<double>(k) + 0.5) * fpc_));
        if (x < kPlotX || x > kNowX)
            continue;
        const auto mid = static_cast<std::int64_t>((static_cast<double>(k) + 0.5) * fpc_);
        const auto ink = taking && mid >= t0 && mid < t1 ? feed_.colour : p.text2;
        const float hi = std::min(1.0f, c->hi), lo = std::max(-1.0f, c->lo);
        const float top = kWaveCy - hi * kWaveHalf, bottom = kWaveCy - lo * kWaveHalf;
        dl.fill({x - 0.9f, top, 1.8f, std::max(1.0f, bottom - top)}, ink);
    }
    // The take region: pending (armed: from the next bar), recording, or looping (track colour).
    const auto state = static_cast<LiveState>(feed_.state);
    std::int64_t r0 = -1, r1 = -1;
    if (state == LiveState::Armed || state == LiveState::Idle)
    {
        r0 = lastBar + static_cast<std::int64_t>(frac < 1e-9 ? 0 : std::llround(bar));
        r1 = r0 + static_cast<std::int64_t>(std::llround(static_cast<double>(feed_.bars) * bar));
    }
    else if (feed_.takeStart >= 0)
    {
        r0 = t0;
        r1 = t1;
    }
    if (r1 > r0)
    {
        const float x0 = std::max(kPlotX, xOf(r0)), x1 = std::min(kPlotX + kPlotW, xOf(r1));
        if (x1 > x0)
        {
            const RectF region {x0, kRegionY, x1 - x0, kRegionH};
            const bool looping = state == LiveState::Looping || state == LiveState::LoopPending;
            dl.fill(region, theme::withOpacity(looping ? feed_.colour : p.rec, 0.12f));
            if (! looping)
                dashedRect(dl, region, 1.5f, p.rec);
            if (state == LiveState::Armed || state == LiveState::Idle)
            {
                char cap[24];
                std::snprintf(cap, sizeof cap, feed_.bars < 1 ? "%.2g BAR" : (feed_.bars == 1 ? "%.0f BAR" : "%.0f BARS"),
                              static_cast<double>(feed_.bars));
                dl.text(x0 + (x1 - x0) / 2, kWaveCy, Align::Centre, TextStyleId::Label, p.text2, cap);
            }
        }
    }
    dl.fill({kNowX - 1, kTop, 2, kBottom - kTop}, p.playhead);
    // The chip right of NOW (countdown, state).
    char chip[40];
    chip[0] = 0;
    if (state == LiveState::Armed || state == LiveState::Idle)
    {
        if (! feed_.playing)
            std::snprintf(chip, sizeof chip, "STARTS WITH PLAY");
        else if (state == LiveState::Armed)
            std::snprintf(chip, sizeof chip, "REC IN %.1f BAR", frac < 1e-9 ? 0.0 : 1.0 - frac);
    }
    else if (state == LiveState::Recording && feed_.takeLength > 0)
    {
        const double done = static_cast<double>(std::clamp<std::int64_t>(now_ - t0, 0, feed_.takeLength)) / bar;
        std::snprintf(chip, sizeof chip, "\xE2\x97\x8F REC %.1f / %g BARS", done, static_cast<double>(feed_.bars));
    }
    if (chip[0] != 0)
    {
        dl.fill({kNowX + 12, 440, 184, 28}, p.surface3, tk::radiusS);
        dl.text(kNowX + 24, 454, Align::Left, TextStyleId::Label, p.rec, chip);
    }
}

void TakeStrip::draw(DrawList& dl, const UiContext& ctx, const TakeStripModel& m)
{
    const auto& p = ctx.palette;
    const auto r = kRect;
    dl.fill(r, p.surface1, tk::radiusS);
    dl.text(r.x + 16, r.y + 18, Align::Left, TextStyleId::Caption, p.text3, "LAST TAKE");
    if (! m.hasTake)
        return;
    const float x0 = r.x + 24, w = r.w - 48;
    const float tx = x0 + w * static_cast<float>(std::clamp(m.take0, 0.0, 1.0));
    const float tw = std::max(4.0f, w * static_cast<float>(std::clamp(m.take1, 0.0, 1.0) - std::clamp(m.take0, 0.0, 1.0)));
    dl.fill({tx, r.y + 28, tw, 28}, theme::withOpacity(m.colour, 0.35f), tk::radiusXs);
    const float nx = tx + w * static_cast<float>(m.nudge);
    dl.stroke({nx, r.y + 28, tw, 28}, p.text1, 2, tk::radiusXs);
    dl.text(tx + tw + 20, r.y + 42, Align::Left, TextStyleId::Caption, p.text2, m.caption);
}
} // namespace ui
