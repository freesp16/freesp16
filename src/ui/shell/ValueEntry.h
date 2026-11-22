// ValueEntry (screens/popups-and-feedback.md "Catalogue"; UID-912, UID-163, UI-123): a double-tap / double-click on a
// knob-strip column opens a 240 × 96 popup above it (x = column x − 16, y 520): the column's label as caption and a
// 208 × 44 field in `value-s` with an accent border and the ↵ hint. Focus = the field with the whole value selected
// (the first typed character replaces it). Digits, '.', '-' and '+' type; ⌫ deletes; Return or push applies; Esc,
// BACK or a tap outside cancels. The edited column stays live (its encoder is bound and still turns the value; the
// field then shows the new value until the next key).
//
// Typed numbers are in the column's display units (our choice): a ParamSpec with its own formatter (VOLUME in dB, PAN
// L/C/R …) is inverted by scanning its range for the raw value whose display reads nearest to the typed number (≤ 4096
// candidates, on Return only); a plain spec takes the number as is. Allocation-free after construction.
#pragma once

#include "core/params/ParamSpec.h"
#include "ui/shell/ParamAccess.h"
#include "ui/shell/Popup.h"
#include "ui/shell/UiState.h"

#include <array>

namespace ui
{
class ValueEntry final : public PopupWidget
{
public:
    ValueEntry(int column0, const core::ParamSpec& spec, std::uint16_t scope, ParamAccess& values);
    RectF bounds() const noexcept override;
    void build(DrawList&, const UiContext&) const override;
    bool handle(const io::ControlEvent&, UiContext&) override;
    bool text(std::string_view utf8) override;
    bool textKey(TextKey) override;
    std::span<const PopupBinding> bindings() const override { return {&binding_, 1}; }

    std::string_view typed() const noexcept { return typed_.view(); }
    bool selected() const noexcept { return selectAll_; }
    // The raw value the typed display number stands for (nearest display reading); false when nothing parses.
    static bool rawFor(const core::ParamSpec&, double typed, float& raw) noexcept;
    void apply();

private:
    int column_;
    const core::ParamSpec& spec_;
    std::uint16_t scope_;
    ParamAccess& values_;
    PopupBinding binding_;
    FixedText<24> typed_;
    bool selectAll_ = true;
};
} // namespace ui
