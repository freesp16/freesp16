// CcCard (07-ui-design/screens/midi-track.md "MIDI CC1 / CC2", UID-803/806/807): one of six 200 × 320 cards
// over the knob columns (x 20 + 208·i, y 152) — caption PARAMETERn, an inner surface-1 panel with the CC number
// (`CC #7`, value-s) and the controller's name in up to two title lines; the ACTIVE toggle 200 × 88 at y 488 (LED
// accent); focused = accent ring + corner triangle (a second tap / SHIFT + push opens the CC NUMBER list); locked on a
// held step = `mod` outline. An inactive card's knob column is `text-3` but stays editable (the screen's StripOwner).
#pragma once

#include "ui/widgets/Widget.h"

#include <cstdint>
#include <string_view>

namespace ui
{
struct CcCardModel
{
    int index = 0;                    // 0–5 (the knob column)
    int cc = -1;                      // the CC number, −1 = a fixed message (Pitch Bend, After Touch)
    std::string_view name1 {}, name2 {}; // the controller's name lines
    bool active = false, focused = false, locked = false;
};

class CcCard
{
public:
    static constexpr RectF card(int i) noexcept { return {20 + 208 * static_cast<float>(i), 152, 200, 320}; }
    static constexpr RectF toggle(int i) noexcept { return {20 + 208 * static_cast<float>(i), 488, 200, 88}; }
    static void draw(DrawList&, const UiContext&, const CcCardModel&);
    // The standard controller names (MIDI 1.0 control change list) split in two lines; empty for unnamed numbers.
    static void nameOf(int cc, std::string_view& line1, std::string_view& line2) noexcept;
};
} // namespace ui
