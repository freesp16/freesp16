// ParamColumn (components/frame.md "ParamColumn"; UID-003, UID-160…166, UID-315, UI-003, UI-203): one of the six
// knob-strip columns, x = 16 + 208·(n−1), 208 wide. Caption = ParamSpec.label (with the 6 lpx mod dot when lockable),
// value = ParamSpec.format, value track 128 × 2 at y 740 (continuous fill / bipolar from the centre / enum ‹ ›).
// States: recently touched (accent underline 600 ms), locked (1.5 lpx mod outline, lock glyph, value in mod), disabled,
// read-only, inactive, sub-readout, FINE while SHIFT is held, empty (nothing drawn, UID-311).
#pragma once

#include "core/params/ParamSpec.h"
#include "ui/widgets/Widget.h"

#include <string_view>

namespace ui
{
struct ParamColumnModel
{
    const core::ParamSpec* spec = nullptr; // null = empty column
    float value = 0;
    bool disabled = false;         // caption + value text-3, encoder ignored (UI-003)
    bool locked = false;           // value locked on the held step (UID-166)
    bool touched = false;          // within 600 ms of the last change (UID-160)
    bool readOnly = false;         // no value track, encoder does nothing
    bool inactive = false;         // text-3 but still editable
    bool fine = false;             // SHIFT held: FINE caption (UID-161)
    std::string_view subReadout{}; // second line in caption style at y 754
    std::string_view valueText{};  // replaces the ParamSpec's format (StripOwner, e.g. LFO SPEED in SYNC)
    bool stepped = false;          // draw the enum ‹ › track (a list walked by its owner)
};

class ParamColumn final : public Widget
{
public:
    explicit ParamColumn(int index0 = 0) noexcept : index_(index0) {}
    void set(const ParamColumnModel& m) noexcept { model_ = m; }
    const ParamColumnModel& model() const noexcept { return model_; }
    RectF bounds() const noexcept override;
    void build(DrawList&, const UiContext&) const override;
    // Whether turning encoder n should change this column.
    bool editable() const noexcept { return model_.spec != nullptr && ! model_.disabled && ! model_.readOnly; }

    static void draw(DrawList&, const UiContext&, int index0, const ParamColumnModel&);

private:
    int index_;
    ParamColumnModel model_;
};
} // namespace ui
