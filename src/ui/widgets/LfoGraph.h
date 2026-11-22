// LfoGraph (07-ui-design/screens/amp-envelope-lfo.md "LFO"; UID-733, UID-738): the LFO screen's graph in the
// 224, 152, 1040 × 456 panel — plot 248, 176, 992 × 392 with the 1 lpx border-strong centre line at y 372, the
// waveform (2 lpx track colour) across the window with DEPTH / 127 of the half-height and PHASE OFFSET shifting its
// start, the `+depth` / `−depth` caption labels, the window grid (free: 4 s with second lines; SYNC: 2 bars with beat
// lines and `BAR n` labels at y 588), the live value (7 lpx playhead dot + 1 lpx 50 % line) while the transport runs,
// a step's locked waveform as a mod dashed overlay, and the NONE / BYPASS states (40 % waveform, caption / badge).
//
// The waveform is cached: setShape() recomputes the 497 column ends only when a parameter changed (the "992-point
// polyline cached per parameter change" budget: one sample every 2 lpx = every canvas pixel of the 800 px panel), and
// build() emits one 2 lpx column per sample pair — ≈ 500 fills, no maths. The shapes are the engine's
// (engine/modulation/Lfo.cpp lfoShapeValue, LFO-001; Random = the xorshift32 sequence of core/util/lfo/LfoLaw.h, one
// value per cycle from the restart), unsmoothed. Message thread; no allocation after construction.
#pragma once

#include "ui/widgets/Widget.h"

#include <array>
#include <cstdint>

namespace ui
{
// One LFO setting as the graph draws it. cycles = LFO cycles across the window (free: Hz × 4 s; SYNC: 8 quarters /
// the division's quarters per cycle).
struct LfoView
{
    int shape = 0;             // lfo.shape 0–6: SINE, TRIANGLE, SAW, SQUARE, PULSE25, PULSE12, RANDOM
    float depth01 = 0.5f;      // lfo.depth / 127
    float phase01 = 0;         // lfo.phase / 128
    float cycles = 1;          // cycles in the window
    std::uint32_t seed = 1;    // Random: core::lfo::seedOf(lfo.randomStart)
    bool oneShot = false;      // ONE SHOT: holds the end value after one cycle
    friend bool operator==(const LfoView&, const LfoView&) = default;
};

class LfoGraph final : public Widget
{
public:
    static constexpr RectF kPanel{224, 152, 1040, 456}, kPlot{248, 176, 992, 392};
    static constexpr int kColumns = 496; // 2 lpx columns across the plot
    static constexpr float kWindowSeconds = 4.0f, kWindowQuarters = 8.0f; // free 4 s, SYNC 2 bars

    // The engine's shape at `phase` (turns, any value) for cycle `cycle` of a Random sequence from `seed`.
    static float shapeValue(int shape, double phase, std::uint32_t seed) noexcept;

    RectF bounds() const noexcept override { return kPanel; }
    void build(DrawList&, const UiContext&) const override;

    // The graph's API, with the window's cycles and the Random seed added.
    void setShape(int shape, float depth01, float phase01, bool sync, double bpm, float cycles,
                  std::uint32_t seed = 1, bool oneShot = false) noexcept;
    void setLocked(const LfoView* locked) noexcept; // null = no overlay
    // The live value: x01 = position in the window, value = the LFO's output −1…+1 (before DEPTH).
    void setLive(float x01, float value, bool on) noexcept;
    void setStyle(std::uint32_t trackColour, int depthLabel, bool noDestination, bool bypassed) noexcept;

    const LfoView& view() const noexcept { return view_; }
    bool sync() const noexcept { return sync_; }
    bool liveOn() const noexcept { return liveOn_; }
    int recomputes() const noexcept { return recomputes_; } // cache misses (tests / bench)
    float yAt(int column) const noexcept { return y_[static_cast<std::size_t>(column)]; }

private:
    static void sample(const LfoView&, std::array<float, kColumns + 1>& out) noexcept;
    void column(DrawList&, std::uint32_t colour, const std::array<float, kColumns + 1>& y, bool dashed) const;

    LfoView view_{}, locked_{};
    bool hasLocked_ = false, sync_ = false, fresh_ = false, freshLocked_ = false;
    double bpm_ = 120;
    std::array<float, kColumns + 1> y_{}, lockedY_{};
    float liveX_ = 0, liveValue_ = 0;
    bool liveOn_ = false;
    std::uint32_t colour_ = 0xFFFFFFFF;
    int depthLabel_ = 63;
    bool noDestination_ = false, bypassed_ = false;
    int recomputes_ = 0;
};
} // namespace ui
