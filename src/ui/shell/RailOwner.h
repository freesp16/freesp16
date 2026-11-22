// Rail items that run screen code (03-layout-grid.md "Rail", components/controls.md "RailItem"; UID-004,
// UI-209): a body widget that also derives from RailOwner takes part in its screen's real rail instead of drawing one
// itself. The shell finds it once per screen entry (dynamic_cast on the body) and asks it, per item:
//   - railActivate: a tap, or a push while the item holds the focus — the screen's action (a popup it builds, an
//     action on its own state). Returning false lets the shell do the descriptor's default (opens / binds).
//   - railState: the item's per-frame state after the shell filled it from `binds` (disabled, on, value text).
//   - railDraw / railRect: an item the screen draws itself and its hit / focus rect (MIXER's FX SETTING card over two
//     slots: MODE and TYPE zones are two items with their own rects, the card drawn with the first).
//   - railFocusEnter: the rotary moved past the rail's last item; returning true hands the focus to the body (its
//     first element, `detents` ≥ 1 steps in), false keeps the focus on the last item.
// Empty rail slots: a RailItemSpec with an empty label is a hole — not drawn, not focusable, never hit; the items
// after it keep their slots (AMP ENVELOPE's slot 1 reserved for PRESET).
#pragma once

#include "ui/descriptors/ScreenSpec.h"
#include "ui/widgets/RailItem.h"
#include "ui/widgets/Widget.h"

namespace ui
{
inline bool isRailHole(const RailItemSpec& s) noexcept
{
    return s.label.empty();
}

class RailOwner
{
public:
    virtual ~RailOwner() = default;
    // x, y: the tap point in canvas lpx, negative for a push.
    virtual bool railActivate(int index, const RailItemSpec& item, float x, float y)
    {
        (void)index, (void)item, (void)x, (void)y;
        return false;
    }
    virtual void railState(int index, const RailItemSpec& item, RailItemState& state) const
    {
        (void)index, (void)item, (void)state;
    }
    virtual bool railDraw(int index, const RailItemSpec& item, const RailItemState& state, DrawList& dl,
                          const UiContext& ctx) const
    {
        (void)index, (void)item, (void)state, (void)dl, (void)ctx;
        return false;
    }
    virtual bool railRect(int index, const RailItemSpec& item, RectF& rect) const
    {
        (void)index, (void)item, (void)rect;
        return false;
    }
    virtual bool railFocusEnter(int detents)
    {
        (void)detents;
        return false;
    }
    // True while the body uses SHIFT + sel.push itself (ARRANGER with ARRANGEMENT on: play from the focused
    // row, 03-ui/control-surface.md SHIFT table, M p.26); the shell's SHIFT + push rail toggle yields.
    virtual bool ownsShiftPush() const { return false; }
};
} // namespace ui
