// SceneKey (components/visuals.md "SceneKey"; screens/scene-manager.md; UID-602, UID-604): one scene or pattern key of
// the SCENE MANAGER (56 × 88 on the PATTERN tab, 56 × 80 on the TRACK tab), surface-2 radius 6:
//   LED bar (w − 16) × 4 at y + 6: off border, has content text-2, current the scene colour, pending half-lit (50 %
//   bar + a full half) blinking at 2 Hz (the caller passes the blink phase in `pending`);
//   the current key a 20 % scene-colour fill; the number in value-s centred (text-1 with content or current, else
//   text-2); a 6 × 4 content dot in the scene colour at the bottom of a key with content (not on the current one);
//   the copied key a 14 lpx copy glyph at the bottom-right (UI-285); focus = the accent ring; `trail` = the 2 lpx
//   border-strong outline the scene key keeps while the focus is in the pattern row; disabled = text-3, no LED.
#pragma once

#include "ui/widgets/Widget.h"

#include <cstdint>

namespace ui
{
struct SceneKeyModel
{
    int index = 1; // 1–16, the number drawn
    std::uint32_t colour = 0;
    bool current = false, pending = false, hasContent = false, focused = false, copied = false;
    bool trail = false, disabled = false;
};

class SceneKey final : public Widget
{
public:
    SceneKey(RectF r = {}, SceneKeyModel m = {}) noexcept : rect_(r), model_(m) {}
    SceneKeyModel& model() noexcept { return model_; }
    RectF bounds() const noexcept override { return rect_; }
    void build(DrawList& dl, const UiContext& ctx) const override { draw(dl, ctx, rect_, model_); }
    static void draw(DrawList&, const UiContext&, RectF, const SceneKeyModel&);

private:
    RectF rect_;
    SceneKeyModel model_;
};
} // namespace ui
