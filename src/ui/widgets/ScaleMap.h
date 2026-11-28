// ScaleMap and PadMap (07-ui-design/screens/playback.md "SET SCALE"; UID-711…713; PAD-010…013, PAD-020).
//   ScaleMap  the degree map panel 224, 152, 304 × 456 (surface-1, radius 8): 16 rows × 4 cells 64 × 24 (4 apart) from
//             (240, 160), top row = highest degrees; in-scale degrees in the track colour at 35 %, octave roots at
//             100 %, the root (degree 0) with an accent corner notch (the manual's dog-ear), the selected key
//             (`selectedDegree`, the manual's 1.6: the key last played from a pad, the root until then) text-1; the
//             pads' 16-degree window framed 2 lpx text-1 (radius 6) on rows 9–12 — the degrees scroll under the fixed
//             frame as AREA pages by 16 (PAD-012). A played degree flashes (pad-flash) when `flashDegree` ≥ 0.
//   PadMap    the 4 × 4 pad map 544, 152, 720 × 448 (cells 168 × 100, 16 apart) in pad order (pad 1 bottom-left,
//             UID-005): label = the semitone offset from the root (`0`, `+2`, value style) or, on MIDI tracks, the
//             note name (`C5`, UID-713); the selected key's pad text-1 with the on-accent label; the root pad an accent
//             corner notch (dog-ear) and `ROOT · PAD 1`; octave pads (and the root when another key is selected) in
//             the track colour; others the track colour at 35 % over surface-2; a hit pad flashes (`flashPad`).
// Pure drawing from the engine's scale tables (core/util/scales); no allocation.
#pragma once

#include "ui/widgets/Widget.h"

#include <cstdint>

namespace ui
{
struct ScaleMapModel
{
    std::uint16_t mask = 0xFFF;   // core::scales::scaleMask(scale.type)
    int area = 0;                 // window (scale.area / scaleMidi.area), −2…+2 pages of 16 degrees
    std::uint32_t colour = 0;     // track colour
    int flashDegree = -1000;      // degree hit, flashing since flashSinceMs
    double flashSinceMs = -1e9;
    int selectedDegree = 0;       // the selected key's degree (core::scales::degreeOf; kNoDegree = none shown)
};

struct PadMapModel
{
    std::uint16_t mask = 0xFFF;
    int area = 0, keyShift = 0;
    bool midi = false;            // note names from rootNote (SET SCALE (MIDI))
    int rootNote = 60;
    std::uint32_t colour = 0;
    int flashPad = -1;            // 0–15
    double flashSinceMs = -1e9;
    int selectedDegree = 0;       // the selected key's degree (core::scales::degreeOf; kNoDegree = none shown)
};

class ScaleMap
{
public:
    static constexpr RectF kPanel{224, 152, 304, 456};
    static constexpr float kX = 240, kY = 160, kCellW = 64, kCellH = 24, kGap = 4, kPitch = kCellH + 4;
    static constexpr int kBaseRow = 12; // row of the window's lowest degrees (rows 9–12 framed)
    static void draw(DrawList&, const UiContext&, const ScaleMapModel&);
    static RectF windowFrame() noexcept;
};

class PadMap
{
public:
    static constexpr RectF kPanel{544, 152, 720, 448};
    static constexpr float kCellW = 168, kCellH = 100, kGap = 16;
    static void draw(DrawList&, const UiContext&, const PadMapModel&);
    static RectF cell(int pad) noexcept; // pad 0 bottom-left
    static int padAt(float x, float y) noexcept; // −1 outside
};
} // namespace ui
