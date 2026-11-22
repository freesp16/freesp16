// GridPopup (components/controls.md "GridPopup"; screens/effects.md "SELECT FX"; UID-216, UI-262): 1184 × 672 at
// (48, 48), title band 56 with ✕, cells 208 × 152 in 5 columns (x 72 + 232·c, y 128 / 304 / 480), 48 lpx icon centred
// at y + 48 and the label at y + 112. Current entry = accent icon + label + 2 lpx accent outline + ✓; focus = accent
// ring + corner triangle; unavailable entries are hidden (the next cells close up). Beyond 15 entries it pages (footer
// pager). Opens on the current entry; turn moves (wrapping around the ends), SHIFT + turn jumps a row, push or one tap
// chooses and closes; BACK cancels. Arrow keys move in 2-D (← → wrap). GridRequest::drawCell draws the icon and any
// per-cell extras with the frame's palette (GridCell).
#pragma once

#include "ui/shell/Popup.h"

namespace ui
{
class GridPopup final : public PopupWidget
{
public:
    static constexpr int kColumns = 5, kRows = 3, kPerPage = kColumns * kRows;
    explicit GridPopup(GridRequest request);
    RectF bounds() const noexcept override;
    void build(DrawList&, const UiContext&) const override;
    bool handle(const io::ControlEvent&, UiContext&) override;
    bool textKey(TextKey) override;

    int focusedItem() const noexcept { return visible_.empty() ? -1 : visible_[static_cast<std::size_t>(focus_)]; }
    int page() const noexcept { return focus_ / kPerPage; }
    int pages() const noexcept { return (static_cast<int>(visible_.size()) + kPerPage - 1) / kPerPage; }
    RectF cell(int slot) const noexcept; // slot on the page 0–14

private:
    void choose();
    GridRequest req_;
    std::vector<int> visible_; // indices of the available items
    int focus_ = 0;            // index into visible_
};
} // namespace ui
