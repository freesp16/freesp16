// ContextBar (components/frame.md "ContextBar", "ContextItem", "MenuButton", "TrackContextBox", "TrackTypeBypass";
// UID-002, UID-115, UID-305, UI-206/207, UI-343): y 40–136 on surface-1 with a 1 lpx border bottom edge. Slot A =
// PROJECT or BACK, B = BPM | QUANTIZE, C = SCENE | PATTERN, D = BAR:BEAT:STEP, E = four menu buttons (TRACKS, MIXER,
// ARRANGER, UTILITY) on top-level screens, the selected track's box on track screens, or TRACK TYPE + BYPASS on the
// TRACK MENU. Hit-testing returns which item a touch lands on; the shell decides what it opens.
#pragma once

#include "ui/descriptors/ScreenSpec.h"
#include "ui/widgets/Widget.h"

#include <string_view>

namespace ui
{
struct ContextBarModel
{
    ContextBarKind kind = ContextBarKind::Menu;
    bool back = false; // slot A shows BACK (PROJECT pages and track screens, UI-343)
    std::string_view bpm = "120.0", quantize = "1/16", scene = "01", pattern = "01", position = "001:01:01";
    bool playing = false;
    bool sceneSelected = false; // SCENE MANAGER open: accent-weak + top bar
    bool sceneDimmed = false;   // SAMPLING/EDIT: SCENE | PATTERN greyed (UI-206)
    int menuSelected = -1;      // 0 TRACKS … 3 UTILITY, -1 none
    int pressed = -1;           // Item pressed (surface-3 for motion.fast)
    // TrackContext
    int track = 1;
    std::string_view trackName{}, typeBadge = "SMPL", stretchBadge{}, loopGlyph{};
    bool missing = false;
    // TrackTypeBypass
    std::string_view trackType = "SAMPLE";
    bool bypass = false;
};

class ContextBar final : public Widget
{
public:
    enum Item : int
    {
        None = -1,
        SlotA = 0,
        Bpm,
        Quantize,
        Scene,
        Pattern,
        Position,
        Menu0,
        Menu1,
        Menu2,
        Menu3,
        TrackBox,
        TrackType,
        Bypass
    };
    void set(const ContextBarModel& m) noexcept { model_ = m; }
    const ContextBarModel& model() const noexcept { return model_; }
    RectF bounds() const noexcept override;
    void build(DrawList&, const UiContext&) const override;
    Item hit(float x, float y) const noexcept; // canvas lpx; 44 lpx hit areas are guaranteed by the 72 lpx items

private:
    ContextBarModel model_;
};
} // namespace ui
