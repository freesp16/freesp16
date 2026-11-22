// SoftwareKeyboard (components/controls.md "Dialog + TextField + SoftwareKeyboard"; screens/project.md "SAVE AS";
// UID-905): 1280 × 256 docked at y 512 over the knob strip, surface-1 with a border top edge. Keys 104 × 52 (gap 8,
// row pitch 60, first row y 528), radius 6, surface-2, body-strong; special keys surface-3:
//   q … p ⌫ · a … l DONE (216, accent, label = the dialog's primary action) · ⇧ z … m - _ . · 123 (160) SPACE (704) ← →
// The 123 layer puts the digits on row 1 and ( ) & @ # + , ' ! on row 2; ⇧ is upper case for the next key (held:
// until pressed again). Layout of project.md + the save-as mockup (the catalogue table in popups-and-feedback.md
// lists an older bottom row).
#pragma once

#include "ui/widgets/Widget.h"

#include <cstdint>
#include <string_view>

namespace ui
{
class SoftwareKeyboard final : public Widget
{
public:
    enum class KeyKind : std::uint8_t
    {
        Char,
        Backspace,
        Done,
        Shift,
        Layer,
        Space,
        Left,
        Right
    };
    struct Key
    {
        KeyKind kind = KeyKind::Char;
        std::string_view text{}; // Char: the UTF-8 text this key types in the current layer/shift state
        RectF rect{};
    };
    static constexpr int kKeyCount = 11 + 10 + 11 + 4;

    RectF bounds() const noexcept override;
    void build(DrawList&, const UiContext&) const override;

    int keyAt(float x, float y) const noexcept; // canvas lpx → key index, -1 = none
    Key key(int index) const noexcept;
    // Applies a key: returns the text to type (Char / Space), or an empty view for the control keys (the dialog handles
    // Backspace, Done, Left, Right through `kind`).
    std::string_view press(int index) noexcept;

    bool shift = false, digits = false, doneEnabled = true;
    int pressedKey = -1;
    double pressedAtMs = -1e9;
    std::string_view doneLabel = "DONE";
};
} // namespace ui
