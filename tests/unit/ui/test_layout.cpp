// Layout constants against 03-layout-grid.md (UID-100…125) and the popup placement rule (UID-901).
#include "ui/shell/Layout.h"

#include <catch2/catch_test_macros.hpp>

namespace L = ui::layout;

TEST_CASE("layout: the four regions keep their order and heights", "[ui][layout]")
{
    CHECK(L::statusRow.y == 0);
    CHECK(L::statusRow.h == 40);
    CHECK(L::contextBar.y == 40);
    CHECK(L::contextBar.h == 96);
    CHECK(L::body.y == 136);
    CHECK(L::body.h == 488);
    CHECK(L::knobStrip.y == 624);
    CHECK(L::knobStrip.h == 144);
    CHECK(L::contentTop == 152);
    CHECK(L::contentBottom == 608);
}

TEST_CASE("layout: every template rect, slot, rail item, column and tile is on the 4 lpx grid (UID-103)",
          "[ui][layout]")
{
    using T = ui::BodyTemplate;
    for (auto t : {T::Full, T::ContentMeter, T::RailContent, T::RailContentMeter, T::RailContentMeters2,
                   T::RailMeterContent, T::Grid})
    {
        const auto b = L::bodyRects(t);
        for (const auto& r : {b.rail, b.content, b.meter, b.meter2})
        {
            CHECK(L::onGrid(r));
            if (r.w > 0)
            {
                CHECK(r.y == 152);
                CHECK(r.x + r.w <= 1264);
            }
        }
        CHECK(L::insideTemplate(t, b.content));
    }
    CHECK(L::bodyRects(T::RailContentMeters2).meter.x == 1080);
    CHECK(L::bodyRects(T::RailMeterContent).meter.x == 224);
    CHECK(L::bodyRects(T::RailMeterContent).content.x == 328);
    for (const auto& r : {L::slotA, L::slotB, L::slotC, L::slotD, L::slotE, L::trackTypeBox, L::bypassBox})
        CHECK(L::onGrid(r));
    for (int i = 0; i < 4; ++i)
    {
        CHECK(L::onGrid(L::menuButton(i)));
        // Rail items (192 × 102 at y 152, 270, 388, 506) and track tiles (278 × 102) are the spec's own exceptions to
        // UID-103 (03-layout-grid.md states them explicitly): checked against the stated values instead.
        CHECK(L::railItem(i).y == 152 + 118 * i);
        CHECK(L::railItem(i).h == 102);
    }
    CHECK(L::menuButton(3).x + L::menuButton(3).w == 1264);
    CHECK(L::railItem(3).y == 506);
    CHECK(L::railItem(4, true).y == 520);
    CHECK(L::railItem(4, true).h == 80);
    for (int n = 0; n < 6; ++n)
        CHECK(L::columnX(n) == 16 + 208 * n);
    // pad order: 13–16 on the top row, 1–4 at the bottom (UID-005)
    CHECK(L::trackTile(13).y == 152);
    CHECK(L::trackTile(1).y == 506);
    CHECK(L::trackTile(1).x == 16);
    CHECK(L::trackTile(16).x == 898);
    CHECK(! L::insideTemplate(T::RailContent, {16, 140, 100, 20}));
}

TEST_CASE("layout: centred popups round down to 4 lpx and never start above y 72 (UID-901)", "[ui][layout]")
{
    const auto list4 = L::centred(640, 48 + 44 * 4);
    CHECK(list4.x == 320);
    CHECK(list4.y == 272);
    const auto setting3 = L::centred(640, 180);
    CHECK(setting3.y == 292);
    const auto tall = L::centred(640, 700);
    CHECK(tall.y == 72);
    CHECK(L::gridPopup.x == 48);
    CHECK(L::modalPanel.x == 88);
    CHECK(L::dialog.x == 208);
    CHECK(L::keyboard.y == 512);
}
