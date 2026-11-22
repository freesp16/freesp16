// Toggle (components/controls.md "Toggle"; UID-251): a Button with an LED bar, a 4 × (w − 32) pill 8 lpx below the top
// edge; off = border-strong, on = accent or the semantic colour the screen names (REC rec, BYPASS / SOFT LIMITER warn,
// ARRANGEMENT play). The label stays text-1. Also the icon toggle (label = an icon, accessible name = the label).
#pragma once

#include "ui/widgets/Widget.h"

#include <cstdint>
#include <string_view>

namespace ui
{
struct ToggleModel
{
    std::string_view label{};
    bool on = false, disabled = false, pressed = false, focused = false;
    std::uint32_t onColour = 0; // 0 = accent
};

class Toggle final : public Widget
{
public:
    Toggle(RectF r = {}, ToggleModel m = {}) noexcept : rect_(r), model_(m) {}
    ToggleModel& model() noexcept { return model_; }
    RectF bounds() const noexcept override { return rect_; }
    void build(DrawList& dl, const UiContext& ctx) const override { draw(dl, ctx, rect_, model_); }
    static void draw(DrawList&, const UiContext&, RectF, const ToggleModel&);

private:
    RectF rect_;
    ToggleModel model_;
};
} // namespace ui
