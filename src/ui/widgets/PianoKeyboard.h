// PianoKeyboard (07-ui-design/screens/midi-track.md, UID-800…811; 03-ui/screens.md MIDI NOTE /
// CC1 / CC2).
//   PianoKeyboard: two octave bands (16 / 648, 152, 616 × 32, caption C4 / C5) over 14 white keys 84 × 296 on an 88
//   pitch and their black keys 52 × 184 (`surface-3`); NOTE in the track colour, NOTE2–4 at 60 % with an index badge;
//   the focused key ring; below, the overview keyboard C0–B9 (70 white keys 16 lpx, `text-1` 85 %, the NOTE key in the
//   track colour) with the shown two octaves framed 2 lpx `text-1` and ‹ ›. Static drawing over a model: the static
//   layer is the same every frame (the renderer's damage diff skips it); only highlights change (≤ 0.2 ms).
#pragma once

#include "ui/widgets/Widget.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace ui
{
struct PianoKeyboardModel
{
    int firstOctave = 4;              // the left band's octave (C0 = MIDI 0, C5 = 60, as the catalogue formats)
    std::array<int, 4> notes {60, -1, -1, -1}; // NOTE, NOTE2–4 (absolute MIDI notes, −1 = OFF)
    int focusedNote = -1;             // the key with the focus ring (−1 = none)
    std::uint32_t colour = 0;         // the track colour
    bool locked = false;              // a held step locks the note: mod outline on the NOTE key
};

class PianoKeyboard
{
public:
    static constexpr RectF kKeys {16, 152, 1248, 336}, kOverview {80, 518, 1120, 76};
    static void draw(DrawList&, const UiContext&, const PianoKeyboardModel&);
    // The MIDI note of a white or black key at canvas (x, y) in the two bands, −1 = none.
    static int noteAt(const PianoKeyboardModel&, float x, float y) noexcept;
    // The x of a white key's left edge (band key index 0–13), for tests.
    static float whiteX(int index) noexcept { return 16.0f + 632.0f * static_cast<float>(index / 7) + 88.0f * static_cast<float>(index % 7) + 2.0f; }
};

} // namespace ui
