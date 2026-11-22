// BigValueField (screens/bpm-quantize.md "BigValueField"; UID-629, UID-630): a focusable value box (surface-2,
// radius 8) with one number in `display` (56) right-aligned at `split` and its fraction or unit in `value-l` text-2
// after it. While it can be edited with the rotary and has the focus it shows the stepper mark at its right edge: a
// 2 lpx border-strong arc with `+` (top), `↕` (accent while focused) and `−` (bottom). Focused = accent ring.
// Disabled (tempo following an external clock): number text-2, no stepper, a source badge (`MIDI`, `LINK`; surface-3,
// caption) at the top-left. A tap on the stepper's upper / lower half steps the value by one detent (stepAt()).
#pragma once

#include "ui/widgets/Widget.h"

#include <string_view>

namespace ui
{
struct BigValueModel
{
    std::string_view whole{}, fraction{}; // "120" + ".0", "50" + "%"
    bool editable = true, focused = false, disabled = false;
    std::string_view badge{}; // source badge while disabled ("MIDI", "LINK")
    float split = 140;        // right edge of the number, from the box's left edge
    float fractionDy = 12;    // the fraction's centre below the number's (value-l sits lower)
    float fractionGap = 4;
};

class BigValueField final : public Widget
{
public:
    BigValueField(RectF r = {}, BigValueModel m = {}) noexcept : rect_(r), model_(m) {}
    BigValueModel& model() noexcept { return model_; }
    RectF bounds() const noexcept override { return rect_; }
    void build(DrawList& dl, const UiContext& ctx) const override { draw(dl, ctx, rect_, model_); }
    static void draw(DrawList&, const UiContext&, RectF, const BigValueModel&);
    // +1 / −1 for a tap on the stepper's upper / lower half, 0 elsewhere (or when the stepper is hidden).
    static int stepAt(RectF r, const BigValueModel&, float x, float y) noexcept;

private:
    RectF rect_;
    BigValueModel model_;
};
} // namespace ui
