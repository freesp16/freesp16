// Layout constants of 07-ui-design/03-layout-grid.md (UID-100…125) on the 1280 × 768 lpx canvas: the four fixed
// regions, the context-bar slots, the body templates, rails, the knob strip and the popup catalogue
// (screens/popups-and-feedback.md). Region heights and the rail width come from the size tokens; paddings from the
// spacing scale (UID-050). Every rect is a multiple of 4 lpx (UID-103; tests/unit/ui/test_layout.cpp checks it).
// Screen packages check their own rects with insideTemplate() in their tests.
#pragma once

#include "ui/descriptors/ScreenSpec.h"
#include "ui/render/DrawList.h"
#include "ui/theme/Theme.h"

#include <array>
#include <cstddef>

namespace ui::layout
{
namespace tk = ::theme::tokens;

inline constexpr float canvasW = tk::canvasWidth, canvasH = tk::canvasHeight;
inline constexpr float pad = tk::space[4]; // 16: region padding, gaps between tiles and rail items
inline constexpr float gap = tk::space[2]; // 8: gaps between context-bar slots and menu buttons

// The four regions (UID-001, UID-110).
inline constexpr float statusY = 0, statusH = tk::sizeStatusRow;               // 0–40
inline constexpr float contextY = statusH, contextH = tk::sizeContextBar;      // 40–136
inline constexpr float stripH = tk::sizeKnobStrip, stripY = canvasH - stripH;  // 624–768
inline constexpr float bodyY = contextY + contextH, bodyH = stripY - bodyY;    // 136–624
inline constexpr float contentTop = bodyY + pad, contentBottom = stripY - pad; // 152–608
inline constexpr RectF statusRow{0, statusY, canvasW, statusH};
inline constexpr RectF contextBar{0, contextY, canvasW, contextH};
inline constexpr RectF body{0, bodyY, canvasW, bodyH};
inline constexpr RectF knobStrip{0, stripY, canvasW, stripH};
inline constexpr RectF canvas{0, 0, canvasW, canvasH};

// Status row (frame.md StatusRow): breadcrumb at x 16 (max 560 wide), notification slot x 320–880, indicators to 1264.
inline constexpr float breadcrumbX = pad, breadcrumbMaxW = 560, statusCentreY = statusH / 2;
inline constexpr float notifyX0 = 320, notifyX1 = 880;
inline constexpr float indicatorsRight = canvasW - pad, indicatorPitch = 40;

// Context bar: controls y 52–124 (72 tall, 12 padding), slots A–E (03-layout-grid.md "Context bar slots").
inline constexpr float ctlY = contextY + tk::space[3], ctlH = 72;
inline constexpr RectF slotA{16, ctlY, 96, ctlH};
inline constexpr RectF slotB{120, ctlY, 256, ctlH}; // BPM 120–248 · QUANTIZE 248–376
inline constexpr RectF slotC{384, ctlY, 256, ctlH}; // SCENE 384–512 · PATTERN 512–640
inline constexpr RectF slotD{648, ctlY, 160, ctlH}; // BAR:BEAT:STEP readout (text at x 664)
inline constexpr RectF slotE{824, ctlY, 440, ctlH};
inline constexpr float menuButtonW = 104, menuPitch = menuButtonW + gap; // 824, 936, 1048, 1160
inline constexpr RectF menuButton(int i) noexcept
{
    return {slotE.x + menuPitch * static_cast<float>(i), ctlY, menuButtonW, ctlH};
}
inline constexpr float pairW = 216; // TRACK TYPE + BYPASS
inline constexpr RectF trackTypeBox{slotE.x, ctlY, pairW, ctlH};
inline constexpr RectF bypassBox{slotE.x + pairW + gap, ctlY, pairW, ctlH};
inline constexpr float readoutX = 664, captionY = 72, valueY = 104; // context items: caption y+20, value y+52

// Body templates (03-layout-grid.md "Body layouts"). Empty rects (w = 0) mean the template has no such column.
struct BodyRects
{
    RectF rail{}, content{}, meter{}, meter2{};
};
inline constexpr float contentH = contentBottom - contentTop; // 456
constexpr BodyRects bodyRects(BodyTemplate t) noexcept
{
    constexpr RectF rail{16, contentTop, 192, contentH};
    constexpr RectF meterR{1176, contentTop, 88, contentH};
    switch (t)
    {
    case BodyTemplate::Full:
        return {{}, {16, contentTop, 1248, contentH}, {}, {}};
    case BodyTemplate::ContentMeter:
        return {{}, {16, contentTop, 1144, contentH}, meterR, {}};
    case BodyTemplate::RailContent:
        return {rail, {224, contentTop, 1040, contentH}, {}, {}};
    case BodyTemplate::RailContentMeter:
        return {rail, {224, contentTop, 936, contentH}, meterR, {}};
    case BodyTemplate::RailContentMeters2:
        return {rail, {224, contentTop, 896, contentH}, {1128, contentTop, 52, contentH}, {1180, contentTop, 84, contentH}};
    case BodyTemplate::RailMeterContent:
        return {rail, {328, contentTop, 936, contentH}, {224, contentTop, 88, contentH}, {}};
    case BodyTemplate::Grid:
        return {{}, {16, contentTop, 1248, contentH}, {}, {}};
    }
    return {};
}
constexpr bool hasRail(BodyTemplate t) noexcept
{
    return bodyRects(t).rail.w > 0;
}

// True when r lies inside the template's content box, rail or meter column (screen packages test their rects).
constexpr bool inside(const RectF& outer, const RectF& r) noexcept
{
    return outer.w > 0 && r.x >= outer.x && r.y >= outer.y && r.x + r.w <= outer.x + outer.w &&
           r.y + r.h <= outer.y + outer.h;
}
constexpr bool insideTemplate(BodyTemplate t, const RectF& r) noexcept
{
    const auto b = bodyRects(t);
    return inside(b.content, r) || inside(b.rail, r) || inside(b.meter, r) || inside(b.meter2, r);
}
constexpr bool onGrid(float v) noexcept
{
    return static_cast<float>(static_cast<int>(v / 4)) * 4 == v;
}
constexpr bool onGrid(const RectF& r) noexcept
{
    return onGrid(r.x) && onGrid(r.y) && onGrid(r.w) && onGrid(r.h);
}

// Rail: four 192 × 102 items at y 152, 270, 388, 506; compact rail: five 192 × 80 items at y 152 … 520 (12 gaps).
inline constexpr std::size_t railMax = 4, railCompactMax = 5;
inline constexpr float railW = tk::sizeRail, railItemH = 102, railCompactH = 80;
constexpr RectF railItem(int i, bool compact = false) noexcept
{
    return compact ? RectF{16, contentTop + (railCompactH + tk::space[3]) * static_cast<float>(i), railW, railCompactH}
                   : RectF{16, contentTop + (railItemH + pad) * static_cast<float>(i), railW, railItemH};
}

// Knob strip: tab x 0–248 at y 624–648; columns x = 16 + 208·(n−1); caption y 660, value 704, track 740 (UID-120).
inline constexpr float tabW = 248, tabTopW = 224, tabH = 24;
inline constexpr float columnX0 = pad, columnW = 208;
inline constexpr float columnCaptionY = 660, columnValueY = 704, columnTrackY = 739, columnSubY = 754;
inline constexpr float columnUnderlineY = 720, separatorY0 = 656, separatorY1 = 752;
constexpr float columnX(int n0) noexcept
{
    return columnX0 + columnW * static_cast<float>(n0);
}
constexpr RectF column(int n0) noexcept
{
    return {columnX(n0), stripY + tabH, columnW, stripH - tabH};
}

// Track tile grid (HOME, TRACK SETTING): 278 × 102 in pad order (UID-005, UID-500).
inline constexpr float tileW = 278, tileH = 102;
constexpr RectF trackTile(int track1Based) noexcept
{
    const int row = 3 - (track1Based - 1) / 4, col = (track1Based - 1) % 4;
    return {16.0f + (tileW + pad) * static_cast<float>(col), contentTop + (tileH + pad) * static_cast<float>(row),
            tileW, tileH};
}

// Popups (03-layout-grid.md "Popups and modals", popups-and-feedback.md "Catalogue").
inline constexpr float popupW = 640, popupTitleH = 48, rowH = tk::sizeHitMin, groupRowH = 36;
inline constexpr int listVisibleRows = 8;
inline constexpr float popupMinY = 72;
constexpr float floor4(float v) noexcept
{
    return static_cast<float>(static_cast<int>(v / 4) * 4);
}
// Centred kinds (UID-901): x = (1280 − w) / 2, y = max(72, (768 − h) / 2), both rounded down to 4 lpx.
constexpr RectF centred(float w, float h) noexcept
{
    const float y = floor4((canvasH - h) / 2);
    return {floor4((canvasW - w) / 2), y < popupMinY ? popupMinY : y, w, h};
}
inline constexpr RectF gridPopup{48, 48, 1184, 672};
inline constexpr float gridTitleH = 56, gridCellW = 208, gridCellH = 152, gridCellGap = 24;
inline constexpr RectF modalPanel{88, 64, 1104, 640};
inline constexpr float modalTitleH = 56;
inline constexpr RectF dialog{208, 56, 864, 320};
inline constexpr float dialogTitleH = 56; // screens/project.md "SAVE AS" (the mockup's band)
inline constexpr RectF textField{256, 164, 768, 56};
inline constexpr RectF dialogHint{256, 236, 768, 20};
inline constexpr float dialogButtonY = 304, dialogButtonW = 240, dialogButtonH = 48;
constexpr RectF dialogButton(int i) noexcept
{
    return {256 + 264.0f * static_cast<float>(i), dialogButtonY, dialogButtonW, dialogButtonH};
}
inline constexpr RectF keyboard{0, 512, canvasW, 256};
inline constexpr float keyH = 52, keyGap = 8, keyRowPitch = 60, keyRow0 = 528;
inline constexpr float messageMinButtonW = 144, buttonH = 48;
inline constexpr float toastX = notifyX0, toastY = 2, toastH = 36, toastMaxW = 480;
inline constexpr float chipH = 28, chipY = 6;
inline constexpr float nestedOffset = tk::space[5]; // 24: a nested ListPopup sits 24 lpx lower and right
inline constexpr float popupRise = tk::space[2];    // 8: popup open "fade + 8 lpx rise"
inline constexpr float screenSlide = tk::space[4];  // 16: screen push/pop slide
} // namespace ui::layout
