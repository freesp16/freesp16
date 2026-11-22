// Scrollbar (components/controls.md "Scrollbar"; UID-904): 4 lpx wide (8 on hover), thumb border-strong, transparent
// track, inset 4 from the right edge, shown only when the content overflows.
#pragma once

#include "ui/widgets/Widget.h"

namespace ui
{
struct ScrollbarModel
{
    float total = 0, visible = 0, first = 0; // rows (or lpx): content size, viewport size, first visible
    bool hover = false;
};

class Scrollbar final : public Widget
{
public:
    Scrollbar(RectF track = {}, ScrollbarModel m = {}) noexcept : rect_(track), model_(m) {}
    ScrollbarModel& model() noexcept { return model_; }
    RectF bounds() const noexcept override { return rect_; }
    void build(DrawList& dl, const UiContext& ctx) const override { draw(dl, ctx, rect_, model_); }
    // `track`: the full height the thumb travels in, x = the bar's left edge.
    static void draw(DrawList&, const UiContext&, RectF track, const ScrollbarModel&);

private:
    RectF rect_;
    ScrollbarModel model_;
};
} // namespace ui
