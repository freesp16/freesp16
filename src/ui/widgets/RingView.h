// RingView (07-ui-design/screens/sampling.md "LIVE SAMPLING", UID-786/787; SMP-020…026): the live
// sampler's ring buffer in the editor area 328, 152, 856 × 360 — the last 6 bars of the input as min/max columns
// (3 lpx pitch, `text-2`) up to a 2 lpx `playhead` NOW line at 75 %, the next 2 bars to its right, bar lines in `play`
// 35 % labelled −6 … −1, NOW, +1; the pending take region (`rec` 12 % with a dashed 1.5 lpx `rec` outline and its
// length caption) from the next boundary, filling with the take's waveform in the track colour while it records; a
// countdown / state chip right of NOW.
//
// Incremental (RingView ≤ 1.0 ms): update() reads only the ring frames written since the last
// call (≤ kChunk per read into a fixed scratch) and folds them into per-column min/max buckets kept in a circular
// cache keyed by the absolute column index (frame / framesPerColumn); build() emits the cached columns. A tempo change
// rebuilds the cache from the ring once. No allocation after construction.
#pragma once

#include "core/util/sampling/CaptureHub.h"
#include "ui/widgets/Widget.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace ui
{
// The live sampler's state the RingView draws, refreshed by its screen every frame (per-block values published by
// the CaptureHub status and the engine's playhead).
struct LiveSamplingFeed
{
    const core::sampling::CaptureRing* ring = nullptr; // null = no ring yet (empty view)
    double sampleRate = 44100;
    double bpm = 120;       // the transport tempo (bar = 4 beats)
    double ppq = 0;         // transport position in quarter notes at the ring's head
    bool playing = false;   // stopped: the view is frozen, the chip reads STARTS WITH PLAY
    int state = 0;          // core::sampling::LiveState
    std::int64_t takeStart = -1, takeLength = 0; // ring frames of the take window (Recording … Looping)
    float bars = 1;         // the record length (live.length) in bars
    std::uint32_t colour = 0; // the target track's colour
};

class RingView
{
public:
    static constexpr RectF kRect {328, 152, 856, 360};
    static constexpr int kColumns = 280, kPast = 210; // 8 bars over 840 lpx at a 3 lpx pitch; NOW at 75 %
    static constexpr int kChunk = 2048;

    void setFeed(const LiveSamplingFeed& f) noexcept { feed_ = f; }
    const LiveSamplingFeed& feed() const noexcept { return feed_; }
    // Folds the frames written since the last call into the column cache (call once per frame, before build).
    void update() noexcept;
    void build(DrawList&, const UiContext&) const;
    void reset() noexcept;

    double framesPerColumn() const noexcept;
    // The ring frame the NOW line stands for (the head), and the x of a ring frame.
    std::int64_t nowFrame() const noexcept { return now_; }
    float xOf(std::int64_t frame) const noexcept;
    // Column cache access (tests, benches): the max |x| of the column holding `frame` (−1 = not cached).
    float peakAt(std::int64_t frame) const noexcept;
    int columnsBuilt() const noexcept { return built_; } // columns folded by the last update()

private:
    struct Column
    {
        std::int64_t index = -1;
        float lo = 0, hi = 0;
    };
    Column& column(std::int64_t index) noexcept { return cache_[static_cast<std::size_t>(index % kCache)]; }
    const Column* find(std::int64_t index) const noexcept;
    void fold(std::int64_t from, std::int64_t to) noexcept;
    static constexpr int kCache = 512;

    LiveSamplingFeed feed_ {};
    std::array<Column, kCache> cache_ {};
    std::array<float, kChunk> l_ {}, r_ {};
    std::int64_t now_ = 0, done_ = -1; // done_: frames folded up to (exclusive)
    double fpc_ = 0;
    int built_ = 0;
};

// TakeStrip (UID-787): the overview area 328, 528, 856 × 64 of LIVE SAMPLING — caption LAST TAKE, the take as a
// track-colour 35 % block in the ring's last 8 bars (right edge = NOW), its saved window framed 2 lpx `text-1` and
// offset by NUDGE, the caption `NUDGE ±n ms`.
struct TakeStripModel
{
    bool hasTake = false;
    double take0 = 0, take1 = 0; // the take's window as fractions of the strip (0 = 8 bars before NOW, 1 = NOW)
    double nudge = 0;            // the nudge as a fraction of the strip
    std::string_view caption {}; // "NUDGE +12 ms"
    std::uint32_t colour = 0;
};
class TakeStrip
{
public:
    static constexpr RectF kRect {328, 528, 856, 64};
    static void draw(DrawList&, const UiContext&, const TakeStripModel&);
};
} // namespace ui
