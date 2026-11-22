// The virtual panel's drawing (07-ui-design/screens/virtual-panel.md "Layout", "Lamps", "States";
// UID-925…936) and its host on a single display surface. The behaviour (layout, hit areas, gestures, lamp mirror) is
// io/controllers/VirtualPanel.h; this draws it into a DrawList at an origin, in panel lpx = canvas lpx (the panel's
// 1280 × 480 canvas has the main canvas's scale, UID-926).
//
// Host (our choice: the device and the 800 × 480 linux window are one surface with no room for the docked drawer —
// virtual-panel.md collapses the drawer below a 0.75 scale): the expanded panel is a drawer over the lower 480 lpx of
// the 1280 × 768 canvas (y 288…768: the screen's top 288 lpx stay visible), the collapsed one its 32 lpx bar along the
// bottom edge; hidden draws nothing and takes no input. Ctrl + \ (KeyRemap::panelToggles) shows / hides it. A screen
// touch (Control::Screen, the logical 800 × 480 point) inside the drawer goes to the panel as pointer 0 and never
// reaches the screen; a touch that started on the panel stays the panel's until it lifts.
#pragma once

#include "io/controllers/VirtualPanel.h"
#include "ui/render/DrawList.h"
#include "ui/theme/Theme.h"

namespace ui::virtualpanel
{
// Draws `panel` with its top-left corner at (ox, oy).
void drawPanel(DrawList& dl, const io::controllers::VirtualPanel& panel, const theme::tk::Palette& p, float ox, float oy,
               double nowMs);

class PanelHost
{
public:
    static constexpr float kCanvasW = 1280, kCanvasH = 768;
    explicit PanelHost(io::controllers::VirtualPanel& panel) : panel_(panel) {}

    // Every event of the frame, before the document and the screens. True = the panel took it (a touch on it).
    bool route(const io::ControlEvent& e, double nowMs);
    // Once per frame after the screens drew: the show / hide key, long-press latches, then the drawing.
    void frame(DrawList& dl, const theme::tk::Palette& p, double nowMs);
    float originY() const noexcept { return kCanvasH - panel_.height(); }

private:
    io::controllers::VirtualPanel& panel_;
    int toggles_ = -1;
    bool tracking_ = false; // pointer 0 went down on the panel
};
} // namespace ui::virtualpanel
