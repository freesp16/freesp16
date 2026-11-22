// The code points every text style of the glyph atlas carries. One list for the atlas (GlyphAtlas.cpp) and
// the font subsetter (resources/fonts/subset.py parses this file): printable ASCII, Latin-1 and Latin Extended-A
// (sample, project and track names, °, ×, µ, ±) and the symbols the spec's screens and mockups use
// (07-ui-design/mockups/*.svg, screens/*.md, components/*.md, 03-ui/*.md) and the widgets draw as text.
// A code point outside this set renders as the fallback glyph (U+FFFD-like box, see GlyphAtlas::find).
#pragma once

#include <cstdint>

namespace ui::fonts
{
struct CodeRange
{
    char32_t first, last;
};
inline constexpr CodeRange kRanges[] = {{0x0020, 0x007E}, {0x00A0, 0x017F}};

inline constexpr char32_t kSymbols[] = {
    0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x2039, 0x203A, // – — ‘ ’ “ ” • … ‹ ›
    0x2190, 0x2191, 0x2192, 0x2193, 0x2194, 0x2195, 0x21A4, 0x21B5, 0x21B6, 0x21E7, // ← ↑ → ↓ ↔ ↕ ↤ ↵ ↶ ⇧
    0x2212, 0x221E, 0x2302, 0x2303, 0x2304, 0x2318, 0x232B, 0x23CE,                 // − ∞ ⌂ ⌃ ⌄ ⌘ ⌫ ⏎
    0x2500, 0x25A0, 0x25A1, 0x25AD, 0x25B2, 0x25B6, 0x25B8, 0x25BC, 0x25BE, 0x25C0, // ─ ■ □ ▭ ▲ ▶ ▸ ▼ ▾ ◀
    0x25CF, 0x25CB, 0x26A0, 0x2713, 0x2715, 0x27F2, 0x266A,                         // ● ○ ⚠ ✓ ✕ ⟲ ♪
};
} // namespace ui::fonts
