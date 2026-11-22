// Shell gaps on the effect screens: open("fx.select") by id shows the populated SELECT FX grid of the slot shown (the
// popup factory of EffectPanel.cpp), and a drag on a knob glyph turns its strip column through the shell (UID-163).
#include "tests/unit/fxui/FxuiTest.h"
#include "ui/shell/Layout.h"

namespace fx = ui::fx;

TEST_CASE("shell.fx: open(\"fx.select\") shows the slot's populated grid and selects into that slot", "[ui][fxui]")
{
    fxuitest::Env e;
    e.open("track.fx1", "chorus");
    REQUIRE(e.f.shell->open("fx.select"));
    e.f.frame();
    auto* g = e.grid();
    REQUIRE(g != nullptr);
    CHECK(g->focusedItem() == 1); // CHORUS, the current effect of FX1 on track 5
    CHECK(e.f.drewText("FX1 \xC2\xB7 TRACK 5"));
    CHECK(e.f.drewText("FILTER"));
    e.f.turn(io::Control::Selector, 3);
    e.f.press(io::Control::SelectorPush);
    e.f.frame();
    CHECK(e.f.shell->topPopup() == nullptr);
    CHECK(e.world.slots.effect(fx::Slot::Fx1, 1, 5) == "filter");
    CHECK(e.f.panel()->effect()->id == "filter"); // the panel rebinds at once
    // FX2's page: the grid targets FX2.
    e.open("track.fx2");
    REQUIRE(e.f.shell->open("fx.select"));
    e.f.frame();
    CHECK(e.f.drewText("FX2 \xC2\xB7 TRACK 5"));
    e.f.press(io::Control::Back);
    CHECK(e.f.shell->topPopup() == nullptr);
}

TEST_CASE("shell.fx: a vertical drag on a knob glyph turns its strip column (UID-163)", "[ui][fxui]")
{
    fxuitest::Env e;
    auto& panel = e.open("track.fx1", "filter");
    const auto* col = e.f.shell->boundColumn(0);
    REQUIRE(col != nullptr);
    const float before = e.f.get(col->id);
    const float x = ui::EffectPanel::knobX(0), y = ui::EffectPanel::kKnobY;
    io::ControlEvent ev{io::ControlEvent::Kind::Touch, io::Control::Screen, ui::touch::kDown, io::kSourceVirtual, 0};
    ev.x = static_cast<std::int16_t>(x / ui::touch::kToCanvas + 0.5f);
    ev.y = static_cast<std::int16_t>(y / ui::touch::kToCanvas + 0.5f);
    e.f.send(ev);
    CHECK(e.f.shell->touchDragColumn() == 0);
    ev.value = ui::touch::kDrag;
    ev.y = static_cast<std::int16_t>(ev.y - 24); // 24 logical px up = 3 detents
    e.f.send(ev);
    ev.value = ui::touch::kUp;
    e.f.send(ev);
    CHECK(e.f.get(col->id) == col->step(before, 3, false));
    CHECK(e.f.get(col->id) != before);
    CHECK(e.f.shell->touchDragColumn() == -1);
    (void)panel;
}
