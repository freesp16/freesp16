// Button (components/controls.md "Button"; UID-340, UID-341): height 48 inline or 102 in rails, radius 6, surface-2,
// label style centred in text-1. pressed = surface-3; disabled = text-3 on surface-1 (place and label kept);
// focused = accent ring; primary = accent fill with on-accent text; destructive = rec text on surface-2.
#pragma once

#include "ui/widgets/Widget.h"

#include <cstdint>
#include <string_view>

namespace ui
{
enum class ButtonStyle : std::uint8_t
{
    Plain,
    Primary,
    Destructive
};

struct ButtonModel
{
    std::string_view label{};
    ButtonStyle style = ButtonStyle::Plain;
    bool disabled = false, pressed = false, focused = false;
    std::string_view line2{}; // two-line rail buttons (kit_track two_line_button)
};

class Button final : public Widget
{
public:
    Button(RectF r = {}, ButtonModel m = {}) noexcept : rect_(r), model_(m) {}
    ButtonModel& model() noexcept { return model_; }
    RectF bounds() const noexcept override { return rect_; }
    void build(DrawList& dl, const UiContext& ctx) const override { draw(dl, ctx, rect_, model_); }
    static void draw(DrawList&, const UiContext&, RectF, const ButtonModel&);

private:
    RectF rect_;
    ButtonModel model_;
};

// A hit rectangle grown to the 44 × 44 minimum around a smaller visual (UID-340).
RectF hitArea(RectF visual) noexcept;
inline bool contains(const RectF& r, float x, float y) noexcept
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}
} // namespace ui
