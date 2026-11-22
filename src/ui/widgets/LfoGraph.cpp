#include "ui/widgets/LfoGraph.h"

#include "core/util/lfo/LfoLaw.h"
#include "ui/theme/Draw.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
constexpr double kTwoPi = 6.283185307179586;
constexpr int kMaxRandomCycles = 4096; // a 40 Hz LFO over 4 s is 160 cycles: far below

double frac(double x) noexcept
{
    return x - std::floor(x);
}
} // namespace

float LfoGraph::shapeValue(int shape, double phase, std::uint32_t seed) noexcept
{
    const double p = frac(phase);
    switch (shape)
    {
    case 0: // SINE
        return static_cast<float>(std::sin(kTwoPi * p));
    case 1: // TRIANGLE: 0 → +1 → −1 → 0, in phase with the sine (engine lfoShapeValue)
        return static_cast<float>(p < 0.25 ? 4.0 * p : (p < 0.75 ? 2.0 - 4.0 * p : 4.0 * p - 4.0));
    case 2: // SAW: falling +1 → −1
        return static_cast<float>(1.0 - 2.0 * p);
    case 3: // SQUARE (pulse 50 %)
        return p < 0.5 ? 1.0f : -1.0f;
    case 4: // PULSE25
        return p < 0.25 ? 1.0f : -1.0f;
    case 5: // PULSE12
        return p < 0.125 ? 1.0f : -1.0f;
    default: // RANDOM: the cycle's held value, one xorshift32 step per cycle from the seed (LFO-008)
    {
        const int cycle = std::clamp(static_cast<int>(std::floor(phase)), 0, kMaxRandomCycles);
        std::uint32_t x = seed != 0 ? seed : 1u;
        for (int k = 0; k <= cycle; ++k)
            x = core::lfo::xorshift32(x);
        return core::lfo::bipolar(x);
    }
    }
}

void LfoGraph::sample(const LfoView& v, std::array<float, kColumns + 1>& out) noexcept
{
    const float centre = kPlot.y + kPlot.h / 2, half = kPlot.h / 2 * std::clamp(v.depth01, 0.0f, 1.0f);
    const double cycles = std::max(0.0f, v.cycles);
    // Random: walk the sequence once instead of per column.
    std::uint32_t state = v.seed != 0 ? v.seed : 1u;
    int cycle = -1;
    float held = 0;
    for (int i = 0; i <= kColumns; ++i)
    {
        double e = cycles * static_cast<double>(i) / kColumns; // cycles since the restart at the window start
        if (v.oneShot && e > 1.0)
            e = 1.0;
        const double total = static_cast<double>(v.phase01) + e;
        float value;
        if (v.shape == 6)
        {
            const int c = std::min(static_cast<int>(std::floor(total)), kMaxRandomCycles);
            while (cycle < c)
            {
                state = core::lfo::xorshift32(state);
                held = core::lfo::bipolar(state);
                ++cycle;
            }
            value = held;
        }
        else
            value = shapeValue(v.shape, total, v.seed);
        out[static_cast<std::size_t>(i)] = centre - value * half;
    }
}

void LfoGraph::setShape(int shape, float depth01, float phase01, bool sync, double bpm, float cycles,
                        std::uint32_t seed, bool oneShot) noexcept
{
    const LfoView v{std::clamp(shape, 0, 6), depth01, phase01, cycles, seed, oneShot};
    sync_ = sync;
    bpm_ = bpm;
    if (fresh_ && v == view_)
        return;
    view_ = v;
    sample(view_, y_);
    fresh_ = true;
    ++recomputes_;
}

void LfoGraph::setLocked(const LfoView* locked) noexcept
{
    hasLocked_ = locked != nullptr;
    if (locked == nullptr || (freshLocked_ && *locked == locked_))
        return;
    locked_ = *locked;
    sample(locked_, lockedY_);
    freshLocked_ = true;
    ++recomputes_;
}

void LfoGraph::setLive(float x01, float value, bool on) noexcept
{
    liveX_ = std::clamp(x01, 0.0f, 1.0f);
    liveValue_ = std::clamp(value, -1.0f, 1.0f);
    liveOn_ = on;
}

void LfoGraph::setStyle(std::uint32_t trackColour, int depthLabel, bool noDestination, bool bypassed) noexcept
{
    colour_ = trackColour;
    depthLabel_ = depthLabel;
    noDestination_ = noDestination;
    bypassed_ = bypassed;
}

// One 2 lpx column per sample pair, spanning both ends (+ the stroke width): a 2 lpx polyline without diagonals.
void LfoGraph::column(DrawList& dl, std::uint32_t colour, const std::array<float, kColumns + 1>& y, bool dashed) const
{
    constexpr float w = 2;
    for (int i = 0; i < kColumns; ++i)
    {
        if (dashed && (i % 6) >= 3) // 6 lpx on, 6 off
            continue;
        const float a = y[static_cast<std::size_t>(i)], b = y[static_cast<std::size_t>(i + 1)];
        const float top = std::min(a, b) - 1, bottom = std::max(a, b) + 1;
        dl.fill({kPlot.x + w * static_cast<float>(i), top, w, bottom - top}, colour);
    }
}

void LfoGraph::build(DrawList& dl, const UiContext& ctx) const
{
    const auto& p = ctx.palette;
    const auto plot = kPlot;
    dl.fill(kPanel, p.surface1, tk::radiusM);
    const float centre = plot.y + plot.h / 2;
    // Window grid: 5 lines at the quarters of the window; SYNC labels the bars, free the seconds.
    for (int k = 0; k <= 4; ++k)
    {
        const float x = plot.x + plot.w * static_cast<float>(k) / 4;
        draw::vline(dl, x, plot.y, plot.bottom(), p.border);
        char lab[12];
        lab[0] = 0;
        if (sync_ && (k == 0 || k == 2))
            std::snprintf(lab, sizeof lab, "BAR %d", k / 2 + 1);
        else if (! sync_ && k < 4)
            std::snprintf(lab, sizeof lab, k == 0 ? "0" : "%d s", k);
        if (lab[0] != 0)
            dl.text(x + 8, plot.bottom() + 20, Align::Left, TextStyleId::Caption, p.text3, lab);
    }
    draw::hline(dl, plot.x, plot.right(), centre, p.borderStrong);
    const bool dim = noDestination_ || bypassed_;
    column(dl, dim ? theme::withOpacity(colour_, 0.4f) : colour_, y_, false);
    if (hasLocked_)
        column(dl, p.mod, lockedY_, true);
    char lab[8];
    std::snprintf(lab, sizeof lab, "+%d", depthLabel_);
    dl.text(plot.x + 8, plot.y + 12, Align::Left, TextStyleId::Caption, p.text3, lab);
    std::snprintf(lab, sizeof lab, "\xE2\x88\x92%d", depthLabel_); // −depth
    dl.text(plot.x + 8, plot.bottom() - 12, Align::Left, TextStyleId::Caption, p.text3, lab);
    if (noDestination_)
        dl.text(plot.x + plot.w / 2, centre - 28, Align::Centre, TextStyleId::Label, p.text3, "NO DESTINATION");
    if (bypassed_) // BYPASS badge top-right of the panel
    {
        const RectF badge{kPanel.right() - 96, kPanel.y + 12, 80, 22};
        dl.fill(badge, p.surface3, tk::radiusXs);
        dl.text(badge.x + badge.w / 2, badge.y + 11, Align::Centre, TextStyleId::Caption, p.warn, "BYPASS");
    }
    if (liveOn_)
    {
        const float x = plot.x + plot.w * liveX_;
        const float y = centre - liveValue_ * plot.h / 2 * std::clamp(view_.depth01, 0.0f, 1.0f);
        dl.fill({x - 0.5f, plot.y, 1, plot.h}, theme::withOpacity(p.playhead, 0.5f));
        dl.fill({x - 7, y - 7, 14, 14}, p.playhead, 7);
    }
}
} // namespace ui
