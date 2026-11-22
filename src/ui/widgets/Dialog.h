// Dialog (components/controls.md "Dialog + TextField + SoftwareKeyboard"; screens/project.md "SAVE AS and name
// dialogs"; UID-905, UI-122): 864 × 320 at (208, 56), title band 56 (centred title, ✕), prompt centred at (640, 140) in
// body text-2, TextField 768 × 56 at (256, 164) with the existing text selected, hint / error line at (256, 236), up to
// three 240 × 48 buttons at x 256 / 520 / 784, y 304 (CANCEL, primary, secondary), and the SoftwareKeyboard docked at
// y 512 over the knob strip (whose DONE key is the primary action). Return = primary, Esc / BACK = CANCEL; a physical
// keyboard types directly; the rotary moves the caret (SHIFT: by word); push = primary. Invalid text: rec border, the
// error in rec under the field, primary + DONE disabled.
#pragma once

#include "ui/shell/Popup.h"
#include "ui/widgets/SoftwareKeyboard.h"
#include "ui/widgets/TextField.h"

#include <string>

namespace ui
{
class Dialog final : public PopupWidget
{
public:
    explicit Dialog(DialogRequest request);
    RectF bounds() const noexcept override;
    void build(DrawList&, const UiContext&) const override;
    bool handle(const io::ControlEvent&, UiContext&) override;
    bool dirty(double) const override { return true; } // caret
    bool coversStrip() const override { return true; }
    bool text(std::string_view utf8) override;
    bool textKey(TextKey) override;
    void cancel() override { finish(req_.cancel); }

    const TextField& field() const noexcept { return field_; }
    SoftwareKeyboard& keyboard() noexcept { return keyboard_; }
    bool valid() const noexcept { return error_.empty(); }
    void finish(int button);

private:
    void revalidate();

    DialogRequest req_;
    TextField field_;
    SoftwareKeyboard keyboard_;
    std::string error_;
};
} // namespace ui
