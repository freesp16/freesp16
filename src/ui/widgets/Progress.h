// Progress (components/controls.md "Progress"; popups-and-feedback.md "Progress rule"; UID-907, UI-008): the blocking
// popup, 640 × 212 (140 without CANCEL), centred, shown only 300 ms after the operation started: title + percentage
// (value-s, text-2, right) at y + 36, detail at y + 80, bar 592 × 8 at y + 104 (accent on border), CANCEL 144 × 48
// bottom-right when cancellable (focused). It closes itself when the job reports done. The inline variant (4 lpx bar
// in a host) is drawInline().
#pragma once

#include "ui/shell/Popup.h"

#include <string>

namespace ui
{
class Progress final : public PopupWidget
{
public:
    explicit Progress(ProgressRequest request);
    RectF bounds() const noexcept override;
    void build(DrawList&, const UiContext&) const override;
    bool handle(const io::ControlEvent&, UiContext&) override;
    bool textKey(TextKey) override;
    bool blocksHome() const override { return true; }
    bool visibleAt(double nowMs) const override;
    void cancel() override; // only when cancellable

    void set(float fraction, std::string_view detail = {});
    void done() noexcept { closed = true; }
    float fraction() const noexcept { return fraction_; }

    static void drawInline(DrawList&, const UiContext&, RectF host, float fraction);

private:
    ProgressRequest req_;
    float fraction_ = 0;
};
} // namespace ui
