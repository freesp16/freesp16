// WaveformEditor (07-ui-design/screens/playback.md; UI-235…239, UI-344, UID-700…710): the PLAYBACK / SET SLICE editor
// 224, 152, 960 × 360 — min/max columns every 3 lpx (exact per column when the overview carries its samples, see
// WaveOverview::ch; otherwise from the pyramid level whose bin is ≤ one column), the START / LOOP START / END flags
// (24 × 28 touch objects) with their lines, the loop band (track colour 12 %), the slice grid (play 18 %, with GRID
// SNAP beats 35 % / bars 70 %), slice boxes (SET SLICE), the 2 lpx playhead, `mod` ghost flags for locked positions
// while a step is held, and the accent ring on the active cursor. Plus the OverviewStrip (224, 528, 960 × 64: whole
// sample, zoom window frame, loop band) and the ZoomGauge (1200, 268, 48 × 128).
// Budget: ≤ 315 editor + 456 overview column rects per redraw (≤ 960), no allocation; the exact columns read
// < 2 finest bins of raw samples each (< 128 frames × channels with the pool's 64-frame bins).
#pragma once

#include "ui/widgets/Widget.h"

#include <array>
#include <cstdint>
#include <span>

namespace ui
{
// The sample's min/max pyramid as the waveform widgets read it. Local view of services::library::Overview
// (SamplePool::overview), so ui does not depend on the library: the composition root adapts one to the other; level k
// holds `bins` min/max pairs of `framesPerBin` frames each (values −1 … +1, mono or the channel max).
struct WaveOverview
{
    struct Level
    {
        const float* min = nullptr;
        const float* max = nullptr;
        std::int64_t bins = 0;
        double framesPerBin = 1;
    };
    std::span<const Level> levels;
    std::int64_t frames = 0;
    // The samples behind the levels (planar, `channels` × `frames`; null = levels only). With them, and levels that
    // form a pyramid (whole-frame finest bin, whole-number steps up), every column is the exact min / max of its own
    // frames: whole bins inside it, the raw samples at its ragged edges and the straight line between samples where
    // it is narrower than a frame — not the bins it touches (a column narrower than the finest bin drew that bin's
    // block, and a transient bled into its neighbours).
    const float* const* ch = nullptr;
    int channels = 0;
};

struct WaveformModel
{
    const WaveOverview* overview = nullptr; // null = empty track (editor and overview surface-1 only)
    std::int64_t frames = 0, start = 0, loopStart = 0, end = 0, play = -1; // play < 0 = idle
    bool loop = false, reverse = false, gridSnap = false;
    int gridCount = 16;
    double zoomCentre = 0.5, zoomX = 1, zoomY = 1; // window = centre ± 0.5 / zoomX of the sample
    int activeCursor = 0;                          // 0 S, 1 L, 2 E, −1 none
    std::array<std::int64_t, 3> lockGhosts{};      // S, L, E positions locked on the held step
    std::uint8_t ghostMask = 0;                    // bit k = ghost k shown
    int slices = 0, currentSlice = 0;              // SET SLICE: slice boxes instead of flags
    // The slice starts in frames plus the sample end (slices + 1 entries; empty = `slices` equal slices /
    // the equal `gridCount` grid). With points, PLAYBACK draws its grid at them (SLC-011) and SET SLICE draws its lines
    // at them and numbered boxes for the AREA's 16 slices from `firstBox` (0-based); bit k of `slicesEdited` = point k
    // was moved or added (its top box gets a 2 lpx outline).
    std::span<const std::int64_t> slicePoints{};
    std::uint64_t slicesEdited = 0;
    int firstBox = 0;
    std::uint32_t colour = 0;
};

class WaveformEditor
{
public:
    static constexpr RectF kEditor{224, 152, 960, 360}, kOverview{224, 528, 960, 64};
    static constexpr float kGaugeX = 1200, kGaugeY = 268;

    static void drawEditor(DrawList&, const UiContext&, RectF r, const WaveformModel&);
    static void drawOverview(DrawList&, const UiContext&, RectF r, const WaveformModel&);
    static void drawZoomGauge(DrawList&, const UiContext&, float x, float y, float level01);
    // Min/max columns of the frames [f0, f1) over `columns` columns of `pitch` lpx starting at x, centred on cy with
    // half-height h; columns whose frames lie outside [in0, in1) are drawn at 35 %.
    static void drawColumns(DrawList&, const WaveOverview&, double f0, double f1, int columns, float x, float pitch,
                            float width, float cy, float h, std::int64_t in0, std::int64_t in1, std::uint32_t colour);
    // The x of frame f in the editor's plot for the model's zoom window.
    static float xOf(const WaveformModel&, RectF plot, std::int64_t f) noexcept;
    static std::int64_t frameAt(const WaveformModel&, RectF plot, float x) noexcept;
    static double windowStart(const WaveformModel&) noexcept;
    static double windowLength(const WaveformModel&) noexcept;
    // A marker flag 24 × 28 with its pointer (right or left), top-left at (x, y).
    static void flag(DrawList&, const UiContext&, float x, float y, std::string_view letter, std::uint32_t fill,
                     std::uint32_t ink, bool pointRight, bool outlineOnly = false);
};
} // namespace ui
