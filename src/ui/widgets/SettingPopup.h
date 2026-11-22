// SettingPopup (components/controls.md "SettingPopup"; popups-and-feedback.md; UID-903, UI-004): a ListPopup with two
// columns (name x + 24, value x + 344, split at x + 320 by a 1 lpx border); the focused row's value cell is
// accent-weak. Turn = rows; push = enter the value side (the cell becomes an inset with an accent border and ‹ ›); turn
// = change (enum ±1, number ± coarse, SHIFT = fine); push = confirm and return to the name side; BACK on the value side
// restores the value; BACK on the name side closes. Opens with the first row focused, name side. A long enum (more
// options than a list shows, kNestedListMin) opens a nested ListPopup instead (24 lpx offset, a second scrim; UID-914):
// choosing there sets and commits the row.
#pragma once

#include "ui/shell/Popup.h"

namespace ui
{
class SettingPopup final : public PopupWidget
{
public:
    static constexpr std::size_t kNestedListMin = 9; // > 8 visible list rows
    explicit SettingPopup(SettingRequest request);
    RectF bounds() const noexcept override;
    void build(DrawList&, const UiContext&) const override;
    bool handle(const io::ControlEvent&, UiContext&) override;
    bool textKey(TextKey) override;

    int focusedRow() const noexcept { return focus_; }
    bool editing() const noexcept { return editing_; }
    const SettingRow& row(int i) const noexcept { return req_.rows[static_cast<std::size_t>(i)]; }
    // A row's shown value changed by its owner (the EDIT list's BAR / BPM follow each other).
    void setValue(int i, float v) noexcept
    {
        if (i >= 0 && i < static_cast<int>(req_.rows.size()))
            req_.rows[static_cast<std::size_t>(i)].value = v;
    }

private:
    void push();
    void back();
    void turn(int detents, bool fine);
    std::string_view valueText(const SettingRow&, char* buf, int n) const;

    SettingRequest req_;
    int focus_ = 0;
    bool editing_ = false;
    int savedOption_ = 0;
    float savedValue_ = 0;
};
} // namespace ui
