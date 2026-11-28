// EnvelopeEditor (07-ui-design/screens/amp-envelope-lfo.md "AMP ENVELOPE"; UI-230…234): the AHR graph in the
// 224, 152, 1040 × 456 panel — plot 256, 184, 976 × 376 (x = time over the fitted window, y = level, VELOCITY at the
// top), time ticks every 1/5 of the window with labels at y 584, the level label at (248, 188), the 2 lpx
// track-colour polyline with a 12 % fill (LINEAR segments straight, CURVE segments bowed), the A / H / R handles
// (28 × 28, `∞` sub-caption at INFINITY), the accent ring on the active handle, the GATE "pad release" marker, the
// locked shape as a dashed mod polyline while a step is held, and a playhead dot.
#pragma once

#include "ui/widgets/Widget.h"

#include <cstdint>

namespace ui
{
enum class EnvCurve : std::uint8_t
{
    Linear,
    Curve
};

struct EnvelopeModel
{
    float attackS = 0, holdS = 0, releaseS = 0, peak = 1; // hold measured from the trigger (includes the attack)
    bool holdInf = true, releaseInf = false, sync = false;
    EnvCurve a = EnvCurve::Linear, r = EnvCurve::Linear;
    float playheadS = -1; // < 0 = none
    int activeHandle = 1; // 0 A, 1 H, 2 R, −1 none
    const EnvelopeModel* lockGhost = nullptr;
    float windowS = 1;  // fitted time window of the plot
    int velocity = 127; // level label
    bool gate = false;  // GATE trigger mode: pad-release marker at the end of hold
    float bpm = 120;    // SYNC on: the time axis in note values at this tempo (UID-728)
    std::uint32_t colour = 0;
};

class EnvelopeEditor
{
public:
    static constexpr RectF kPanel{224, 152, 1040, 456}, kPlot{256, 184, 976, 376};
    static void draw(DrawList&, const UiContext&, const EnvelopeModel&);
    // Handle top-left positions (A, H, R) for hit tests.
    static RectF handleRect(const EnvelopeModel&, int handle) noexcept;
    // The window that puts the release end at ≈ 85 % of the plot, rounded to 100 ms / 1 s / 5 s steps.
    static float fitWindow(const EnvelopeModel&) noexcept;
    // x of time s, y of level 0–1 in the plot.
    static float xOf(const EnvelopeModel& m, float s) noexcept;
    // A small envelope thumbnail (TRACK MENU card): shape only, in `r`. Each corner sits on its own knob's scale, not
    // on a fitted window, so a detent moves only the corners its value moves, in its direction; at RELEASE INFINITY
    // the level stays at the peak (no fall drawn).
    static void drawThumb(DrawList&, const UiContext&, RectF r, const EnvelopeModel&);
};
} // namespace ui
