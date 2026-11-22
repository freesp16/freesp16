// DataTable (07-ui-design/screens/arranger.md "Layout", UID-576): a table panel of fixed columns — a header
// band (48 lpx, surface-3, captions with an optional text-3 sub-caption), 44 lpx rows in zebra (surface-2 /
// surface-1), the ListRow focus (accent-weak fill + 3 lpx accent bar, body-strong text), a 3 lpx `play` bar on the
// playing row, an optional terminal row (a centred label between two 80 lpx border-strong rules; focusable, always
// last) and a 4 lpx scrollbar. Virtualised: the screen asks for the visible window (first, visible) and draws only
// those rows' cells (cellText / swatch helpers), so 999 rows cost what 9 do (9 visible rows ≤
// 0.6 ms). Static drawing over a model; no state, no allocation.
#pragma once

#include "ui/widgets/Widget.h"

#include <span>
#include <string_view>

namespace ui
{
struct DataColumn
{
    float x = 0, w = 0;            // relative to the panel
    std::string_view header {}, sub {};
};

struct DataTableModel
{
    RectF panel {224, 152, 936, 456};
    std::span<const DataColumn> columns {};
    int rows = 0;          // data rows (the terminal row, when present, is row `rows`)
    bool terminal = false; // a terminal row after the data rows
    std::string_view terminalText {};
    float terminalCentre = 0; // x of the terminal label (0 = the panel's centre)
    int first = 0;            // first visible row (absolute)
    int focused = -1, playing = -1;
    float headerH = 48, rowH = 44;
    int visible = 9;
    float scrollbarX = 0; // absolute x of the scrollbar (0 = panel right − 8)
};

class DataTable
{
public:
    // Panel, header band and captions, scrollbar.
    static void drawFrame(DrawList&, const UiContext&, const DataTableModel&);
    // Row `row`'s background (zebra, play bar, focus) at its visible slot; returns the row's rect (h = rowH).
    static RectF drawRow(DrawList&, const UiContext&, const DataTableModel&, int row);
    // The terminal row's label and rules (call after drawRow(rows)).
    static void drawTerminal(DrawList&, const UiContext&, const DataTableModel&, RectF rowRect);
    // A centred cell text in column `c` (text-1 / body or body-strong on the focused row unless ink / style given).
    static void cell(DrawList&, const DataTableModel&, const theme::tk::Palette&, RectF rowRect, int c,
                     std::string_view text, std::uint32_t ink, bool strong, float dx = 0, Align align = Align::Centre);
    // Total rows including the terminal one.
    static int count(const DataTableModel& m) noexcept { return m.rows + (m.terminal ? 1 : 0); }
    // The first visible row that keeps `focused` in view with one row of context (UID-170).
    static int scrollFor(int focused, int first, int count, int visible) noexcept;
    // The row under canvas point y (−1 = none).
    static int rowAt(const DataTableModel&, float x, float y) noexcept;
};
} // namespace ui
