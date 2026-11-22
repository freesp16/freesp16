// ListRow (components/controls.md "ListRow"; popups-and-feedback.md "ListRow states inside popups"): 44 tall, body text
// at x + 24, optional value column (from 50 % of the width, body text-2), trailing › (opens) or ✓ (current value).
// Zebra: even rows surface-2, odd surface-1. Focused: accent-weak + 3 lpx accent left bar + body-strong. Current value:
// accent text + ✓ at x + w − 32. Disabled: text-3, skipped by the rotary but kept in place. Group header: 36 tall,
// surface-3, caption text-2, never focused. Editing: the value cell becomes an inset with an accent border and ‹ ›.
#pragma once

#include "ui/widgets/Widget.h"

#include <string_view>

namespace ui
{
struct ListRowModel
{
    std::string_view label{}, value{};
    float valueX = -1; // value column offset from the row's x; < 0 = 50 % of the width
    int zebra = 0;     // row index for the zebra fill
    bool header = false, focused = false, current = false, disabled = false, opens = false, editing = false;
    float indent = 0;  // TreeRow / grouped list indentation
    float labelX = 24; // 72 with an icon column
};

class ListRow final : public Widget
{
public:
    ListRow(RectF r = {}, ListRowModel m = {}) noexcept : rect_(r), model_(m) {}
    ListRowModel& model() noexcept { return model_; }
    RectF bounds() const noexcept override { return rect_; }
    void build(DrawList& dl, const UiContext& ctx) const override { draw(dl, ctx, rect_, model_); }
    static void draw(DrawList&, const UiContext&, RectF, const ListRowModel&);

private:
    RectF rect_;
    ListRowModel model_;
};
} // namespace ui
