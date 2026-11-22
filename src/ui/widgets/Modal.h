// Modal (panel) (components/controls.md "Modal"; popups-and-feedback.md): 1104 × 640 at (88, 64), radius 12,
// surface-1, title band 56 with ✕ and a hairline; the content is a body widget (PopupSpec::bodyComponent, e.g. the
// BPM/QUANTIZE panel) laid out in labelled sections (draw::captionLine). The modal's own knob-strip columns
// are its bindings; while it is open the encoders edit them (UID-125, "encoders are swallowed only while … Modal").
#pragma once

#include "ui/shell/Popup.h"

#include <memory>

namespace ui
{
class Modal final : public PopupWidget
{
public:
    Modal(std::string_view title, std::unique_ptr<Widget> body, std::span<const PopupBinding> bindings = {});
    RectF bounds() const noexcept override;
    RectF content() const noexcept; // inside the panel below the title band
    void build(DrawList&, const UiContext&) const override;
    bool handle(const io::ControlEvent&, UiContext&) override;
    bool dirty(double nowMs) const override { return body_ && body_->dirty(nowMs); }
    std::span<const PopupBinding> bindings() const override { return bindings_; }
    bool swallowsEncoders() const override { return ! bindings_.empty(); }
    Widget* body() noexcept { return body_.get(); }

private:
    std::string_view title_;
    std::unique_ptr<Widget> body_;
    std::span<const PopupBinding> bindings_;
};
} // namespace ui
