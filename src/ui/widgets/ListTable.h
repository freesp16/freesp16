// ListTable (07-ui-design/screens/browse.md "List panel", "Rows"; UI-310…315, UID-675…686): the BROWSE list panel
// 224, 152, 1040 × 456 — a title band (title, free space, sort glyph), 9 visible 44 lpx TreeRows from y 200 (zebra,
// indentation 24 per level from x 248, 1 lpx connector lines, ▸ / ▾ disclosure, folder / file icon, name; root badge;
// USB mark; ✓ + accent on the assigned file; `▶ PREVIEW` on the focused file while it plays; an inline 4 lpx accent
// progress bar under a loading row) and a scrollbar when rows overflow. Virtualised: only the visible rows are built,
// whatever the list length.
#pragma once

#include "ui/widgets/Widget.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace ui
{
struct TreeRowModel
{
    enum class Kind : std::uint8_t
    {
        ClosedFolder,
        OpenFolder,
        File,
        Message
    };
    int level = 0;
    Kind kind = Kind::File;
    std::string_view name{}, badge{}, value{}; // value: TEXT results' folder path (x 744)
    bool usb = false, assigned = false, disabled = false;
};

struct ListTableModel
{
    std::string_view title = "FOLDER", freeSpace{}, sort{};
    std::span<const TreeRowModel> rows;
    int first = 0, focused = -1; // first visible row, focused row (absolute indices)
    bool previewing = false;     // ▶ PREVIEW after the focused file
    float loading = -1;          // progress 0–1 of the focused row's load, < 0 none
    int loadingRow = -1;
};

class ListTable
{
public:
    static constexpr RectF kPanel{224, 152, 1040, 456};
    static constexpr float kRowY0 = 200, kRowH = 44;
    static constexpr int kVisible = 9;
    static void draw(DrawList&, const UiContext&, const ListTableModel&);
    static void drawRow(DrawList&, const UiContext&, float y, int zebra, const TreeRowModel&, bool focused,
                        bool previewing);
    // The first visible row that keeps one row of context around `focused` (UID-170).
    static int scrollFor(int focused, int first, int count) noexcept;
};
} // namespace ui
