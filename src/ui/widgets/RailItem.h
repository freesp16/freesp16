// RailItem (components/controls.md "RailItem"; UID-004, 03-layout-grid.md "Rail"): a Button, Toggle or OptionSelector
// at rail size 192 × 102 (compact rail: 192 × 80), placed from a ScreenSpec's RailItemSpec. Fewer items leave the
// bottom empty; nothing stretches.
#pragma once

#include "ui/descriptors/ScreenSpec.h"
#include "ui/widgets/Widget.h"

#include <cstdint>
#include <string_view>

namespace ui
{
struct RailItemState
{
    bool on = false, disabled = false, pressed = false, focused = false;
    std::string_view value{};   // Option: current value text
    std::uint32_t onColour = 0; // Toggle: semantic LED colour (0 = accent)
};

class RailItem final : public Widget
{
public:
    RailItem(const RailItemSpec* spec = nullptr, int index = 0, bool compact = false) noexcept
        : spec_(spec), index_(index), compact_(compact)
    {
    }
    const RailItemSpec* spec() const noexcept { return spec_; }
    RailItemState& state() noexcept { return state_; }
    const RailItemState& state() const noexcept { return state_; }
    RectF bounds() const noexcept override;
    void build(DrawList&, const UiContext&) const override;

private:
    const RailItemSpec* spec_;
    int index_;
    bool compact_;
    RailItemState state_;
};
} // namespace ui
