// Toast (components/controls.md "Toast"; popups-and-feedback.md "Toast"; UID-908, UID-916): a 36 lpx pill (surface-3,
// radius 18) in the status-row notification slot at (320, 2), ≤ 480 wide, optional 16 lpx glyph, body text; hides
// after 2 s with a `fast` fade; a toast carrying a job's inline bar stays until the job ends. Never blocks input,
// never takes focus; drawn above the scrim. A click dismisses it (or opens its target).
#pragma once

#include "ui/shell/Popup.h"

namespace ui
{
class Toast final : public PopupWidget
{
public:
    explicit Toast(ToastRequest request);
    RectF bounds() const noexcept override { return rect_; }
    void build(DrawList&, const UiContext&) const override;
    bool handle(const io::ControlEvent&, UiContext&) override;
    bool dirty(double nowMs) const override;
    bool modal() const override { return false; }

    void setProgress(float f) noexcept { req_.progress = f; }
    // Called every frame by the shell: measures the pill once and hides the toast when its time is up.
    void tick(double nowMs, const TextMeasure&);
    float opacity(double nowMs) const noexcept;

    // Caution chip (popups-and-feedback.md "Caution banner", UID-909): (text + 48) × 28 pill, surface-2, 1 lpx warn
    // border, warn ⚠ glyph 16, caption text-1; chips end at x 880, 8 apart. Returns the chip's left edge.
    static float drawCautionChip(DrawList&, const UiContext&, float right, std::string_view text);

private:
    ToastRequest req_;
    RectF rect_{};
    double doneAtMs_ = -1;
};
} // namespace ui
