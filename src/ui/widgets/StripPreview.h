// StripPreview (07-ui-design/screens/utility.md "StripPreview (local component)"; UID-884; RIB-006): the
// focused TOUCH STRIP SETTING place's mapping over the strip's eight zones — surface-1 panel 1048, 152, 216 × 456
// (radius 8), caption `STRIP PREVIEW`, `TOP` + the top value (value-s), eight 48 × 30 zones stacked bottom → top (the
// strip's LEDs), filled in accent with an opacity that follows the mapped value from the BOTTOM side to the TOP side,
// `BOTTOM` + the bottom value; while the strip is touched in this USER mode the zones light as the strip's LED bar
// graph up to the touched zone (strongest at the bottom end, as touch-strip-setting.svg) under a 2 lpx playhead line.
// Read-only. No allocation.
#pragma once

#include "ui/widgets/Widget.h"

#include <string_view>

namespace ui
{
struct StripPreviewModel
{
    std::string_view topText{}, bottomText{}; // the target's formatted TOP / BOTTOM values ("—" on an empty place)
    float top01 = 1, bottom01 = 0;            // normalised range of the target (RibbonItem)
    bool empty = true;                        // no target: zones dark
    int livePosition = -1;                    // 0–127 while the strip is touched (any mode), else −1
};

class StripPreview
{
public:
    static constexpr RectF kPanel{1048, 152, 216, 456};
    static constexpr float kZoneW = 48, kZoneH = 30, kZonePitch = 38, kZoneTop = 248;
    static void draw(DrawList&, const UiContext&, const StripPreviewModel&);
    static RectF zone(int zoneFromBottom) noexcept; // 0 = bottom … 7 = top
};
} // namespace ui
