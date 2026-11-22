// ListPopup (components/controls.md "ListPopup"; popups-and-feedback.md; UID-902, UID-904, UI-004, UI-341): 640 wide,
// title band 48 + rows of 44 (max 8 visible, then a Scrollbar), centred (UID-901). Opens with the current value focused
// (accent + ✓); turn moves the focus one row (no wrap, skipping group headers and disabled rows, auto-scroll with one
// row of context); SHIFT + turn jumps by group; push or a click on the focused row chooses and closes; BACK, ✕ or a
// click outside closes without change. Keyboard: ↑ ↓ PgUp PgDn Home End, letters jump, Return, Esc. Rows may carry a
// TrackChip before the label (ListRequest::chips, UID-842: the SOURCE list).
// Group headers take one 44 lpx slot inside popups, as the spec's mockups draw them (lfo-destination); the
// 36 lpx header of controls.md is used by inline lists.
#pragma once

#include "ui/shell/Popup.h"

#include <vector>

namespace ui
{
class ListPopup final : public PopupWidget
{
public:
    explicit ListPopup(ListRequest request);
    RectF bounds() const noexcept override;
    void build(DrawList&, const UiContext&) const override;
    bool handle(const io::ControlEvent&, UiContext&) override;
    bool textKey(TextKey) override;
    bool text(std::string_view utf8) override;
    void cancel() override;

    int focusedItem() const noexcept; // index into the request's items, -1 = none
    int firstVisibleRow() const noexcept { return first_; }
    int rowCount() const noexcept { return static_cast<int>(rows_.size()); }
    void choose();

private:
    struct Row
    {
        int item; // -1 = group header
        std::string_view label;
    };
    bool focusable(int row) const noexcept;
    bool hasChip(int item) const noexcept
    {
        return item >= 0 && item < static_cast<int>(req_.chips.size()) &&
               req_.chips[static_cast<std::size_t>(item)].track > 0;
    }
    void moveFocus(int detents, bool byGroup);
    void scrollToFocus();
    int rowAt(float x, float y) const noexcept;

    ListRequest req_;
    std::vector<Row> rows_;
    int focus_ = -1, first_ = 0;
    bool chosen_ = false;
};
} // namespace ui
