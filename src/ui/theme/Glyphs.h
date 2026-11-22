// Line glyphs and icons of the Sixteen icon family (07-ui-design/components/visuals.md "Icons", UID-018: 2 lpx stroke,
// rounded, drawn on a 24 lpx grid), built from the DrawList's rounded rectangles: a diagonal stroke is a run of small
// squares one lpx apart (exact at every scale ≥ 0.5 because neighbours overlap), circles are fully rounded rects.
// Used for the glyphs the glyph atlas does not carry (✕ ✓ ⌄ ⌫ ⏎ ⇧ ← lock, info/caution/error) and for the frame icons
// (PROJECT, BACK, TRACKS, MIXER, ARRANGER, UTILITY). Coordinates are canvas lpx; colours are palette tokens.
#pragma once

#include "ui/render/DrawList.h"

#include <cstdint>

namespace ui::glyph
{
enum class Kind : std::uint8_t
{
    Close,       // ✕ (title bands)
    Check,       // ✓ (current value)
    ChevronDown, // ⌄ (OptionSelector)
    Backspace,   // ⌫
    Return,      // ⏎ / ↵
    Shift,       // ⇧
    ArrowLeft,   // ←
    ArrowRight,  // →
    Lock,        // padlock, 12 lpx (UID-166)
    Info,        // message glyphs (popups-and-feedback.md), 24 grid
    Caution,
    Error,
    Usb,
    Project, // context bar slot A icons (frame.md ContextItem)
    Back,
    Tracks, // menu button icons
    Mixer,
    Arranger,
    Utility
};

// A straight stroke of `width` lpx from (x1, y1) to (x2, y2) with round caps.
void line(DrawList&, float x1, float y1, float x2, float y2, float width, std::uint32_t argb);

// Draws `kind` with its 24-grid origin at (x, y), scaled so the grid is `size` lpx wide.
void draw(DrawList&, Kind kind, float x, float y, float size, std::uint32_t argb);
} // namespace ui::glyph
