// Message (components/controls.md "Message"; popups-and-feedback.md; UID-906, UI-296): 640 × (72 + 20·lines + 96),
// centred; glyph 32 at (x + 24, y + 20) (info accent / caution warn / error rec), title at (x + 72, y + 36), body lines
// in text-2 from y + 80, buttons 48 tall (min 144 wide) right-aligned with 16 gaps and 24 bottom padding. The default
// (push / Return) is the affirmative button; BACK / Esc is always CANCEL; a destructive affirmative is rec text.
#pragma once

#include "ui/shell/Popup.h"

namespace ui
{
class Message final : public PopupWidget
{
public:
    explicit Message(MessageRequest request);
    RectF bounds() const noexcept override;
    void build(DrawList&, const UiContext&) const override;
    bool handle(const io::ControlEvent&, UiContext&) override;
    bool textKey(TextKey) override;
    void cancel() override { press(req_.cancelButton); }

    int focusedButton() const noexcept { return focus_; }
    RectF buttonRect(int i, const TextMeasure&) const noexcept;
    void press(int button);

private:
    MessageRequest req_;
    int focus_ = 0;
    TextMeasure measure_; // cached from the first build for hit-testing
};
} // namespace ui
