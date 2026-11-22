// OptionSelector (components/controls.md "OptionSelector"): surface-2 box (radius 8; 102 tall in rails, 80 in option
// rows) with a caption in text-2 and an inset value box (surface-1, body-strong, ⌄ chevron in text-3). Push / tap opens
// the matching ListPopup (the shell does that from the RailItemSpec `opens` / `binds`).
#pragma once

#include "ui/widgets/Widget.h"

#include <string_view>

namespace ui
{
struct OptionSelectorModel
{
    std::string_view caption{}, value{};
    bool disabled = false, pressed = false, focused = false;
};

class OptionSelector final : public Widget
{
public:
    OptionSelector(RectF r = {}, OptionSelectorModel m = {}) noexcept : rect_(r), model_(m) {}
    OptionSelectorModel& model() noexcept { return model_; }
    RectF bounds() const noexcept override { return rect_; }
    void build(DrawList& dl, const UiContext& ctx) const override { draw(dl, ctx, rect_, model_); }
    static void draw(DrawList&, const UiContext&, RectF, const OptionSelectorModel&);

private:
    RectF rect_;
    OptionSelectorModel model_;
};
} // namespace ui
