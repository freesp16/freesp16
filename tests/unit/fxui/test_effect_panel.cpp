// The generic EffectPanel (screens/effects.md; UID-825…842, UI-260…268): everything is derived from the descriptor and
// the ParamSpecs — knob columns in page order, option cells and control kinds, the strip binding with its locks, SYNC
// columns, the MIX hint, NONE, the bus page and its FX modes, BYPASS, rotary/keyboard focus.
#include "tests/unit/fxui/FxuiTest.h"
#include "ui/shell/Layout.h"

using namespace fxuitest;
namespace fx = ui::fx;
using Control = ui::EffectPanel::Control;

TEST_CASE("fxui.panel_columns: knob n is the descriptor's page-order parameter for the slot (UID-826, FX-067)",
          "[fxui]")
{
    Env e;
    auto& w = e.world;
    for (const auto& def : w.defs)
        for (const auto slot : {fx::Slot::Fx1, fx::Slot::Fx2, fx::Slot::Send, fx::Slot::Master})
        {
            if (! engine::fx::allows(def.slots, fx::kind(slot)) || def.id == "none")
                continue;
            const auto* d = w.effects.find(def.id);
            REQUIRE(d != nullptr);
            for (std::size_t n = 0; n < 6; ++n)
            {
                const auto* s = fx::resolveParam(w.params, slot, *d, d->params[n]);
                INFO(def.id << " " << fx::prefix(slot) << " column " << n + 1);
                if (def.knobs[n].key.empty())
                    CHECK(s == nullptr); // an empty position stays empty
                else
                {
                    REQUIRE(s != nullptr);
                    CHECK(s->id.substr(0, fx::prefix(slot).size()) == fx::prefix(slot));
                    CHECK(s->label == def.knobs[n].label);
                    CHECK(s->lockable == fx::isInsert(slot)); // FX-078 / FX-094
                }
            }
        }
}

TEST_CASE("fxui.panel_strip: the strip is rebound to the slot's ParamSpecs with the slot tab (UID-125)", "[fxui]")
{
    Env e;
    auto& panel = e.open("track.fx2", "lofi", fx::Slot::Fx1);
    CHECK(panel.slot() == fx::Slot::Fx2);
    CHECK(panel.effect()->id == "none"); // FX2 is still empty: LO-FI went to FX1
    CHECK(e.f.shell->stripTab() == "FX2");
    for (int n = 0; n < 6; ++n)
        CHECK(e.f.shell->boundColumn(n) == nullptr); // NONE: an empty strip with its tab (UID-841)
    e.world.slots.selectEffect(fx::Slot::Fx2, 1, 5, "filter");
    e.f.frame();
    e.f.frame();
    CHECK(e.f.shell->boundColumn(0) == &e.world.spec("fx2.filter.cutoff"));
    CHECK(e.f.shell->boundColumn(2) == &e.world.spec("fx2.filter.q"));
    CHECK(e.f.shell->boundColumn(4) == nullptr);
    CHECK(e.f.shell->boundColumn(5) == &e.world.spec("fx2.filter.mix"));
    // Encoder 1 edits CUT OFF of track 5's FX2; with step 3 held it writes a lock instead (UID-829).
    const float before = e.f.get("fx2.filter.cutoff");
    e.f.turn(io::knob(1), 5);
    CHECK(e.f.get("fx2.filter.cutoff") > before);
    e.f.press(io::step(3));
    e.f.turn(io::knob(1), -10);
    float lock = 0;
    const auto& cutoff = e.world.spec("fx2.filter.cutoff");
    CHECK(e.f.shell->values().lockOf(cutoff, e.f.shell->state().trackScope(5), 3, lock));
    e.f.frame();
    CHECK(e.f.shell->stripTab() == "FX2");
    CHECK(e.f.shell->column(0).locked);
    e.f.release(io::step(3));
}

TEST_CASE("fxui.panel_bus_locks: bus columns carry no lock and are disabled while a step is held (UID-829)", "[fxui]")
{
    Env e;
    e.world.slots.selectEffect(fx::Slot::Send, 1, 0, "reverb");
    auto& panel = e.open("mixer.busFx", {}, fx::Slot::Send);
    CHECK(panel.effect()->id == "reverb");
    CHECK(e.f.shell->stripTab() == "SEND FX");
    CHECK_FALSE(e.f.shell->boundColumn(0)->lockable);
    e.f.press(io::step(2));
    e.f.frame();
    for (int n = 0; n < 6; ++n)
        CHECK(e.f.shell->column(n).disabled);
    const float v = e.f.get("send.reverb.time");
    e.f.turn(io::knob(3), 4);
    CHECK(e.f.get("send.reverb.time") == v);
    e.f.release(io::step(2));
    e.f.turn(io::knob(3), 4);
    CHECK(e.f.get("send.reverb.time") > v);
}

TEST_CASE("fxui.panel_options: option cells keep the reference cells and derive their control (UID-827, UID-842)",
          "[fxui]")
{
    Env e;
    auto cellOf = [](const ui::EffectPanel& p, std::string_view key)
    {
        for (int i = 0; i < 6; ++i)
            if (const auto& o = p.option(i); o.spec != nullptr && o.spec->id.ends_with(key))
                return std::pair{o.cell + 1, o.control};
        return std::pair{-1, Control::None};
    };
    auto& panel = e.open("track.fx1", "filter");
    CHECK(cellOf(panel, ".type") == std::pair{1, Control::Selector});
    CHECK(cellOf(panel, ".slope") == std::pair{2, Control::Segmented});
    e.world.slots.selectEffect(fx::Slot::Fx1, 1, 5, "compressor");
    e.f.frame();
    CHECK(cellOf(panel, ".lookAhead").first == -1); // not on inserts: cell 1 stays empty (OQ-15)
    CHECK(cellOf(panel, ".detect") == std::pair{2, Control::Segmented});
    CHECK(cellOf(panel, ".knee") == std::pair{3, Control::Toggle});
    e.world.slots.selectEffect(fx::Slot::Fx1, 1, 5, "ducker");
    e.f.frame();
    CHECK(cellOf(panel, ".sync") == std::pair{1, Control::Toggle});
    CHECK(cellOf(panel, ".source") == std::pair{2, Control::Selector});
    e.world.slots.selectEffect(fx::Slot::Fx1, 1, 5, "eq2");
    e.f.frame();
    CHECK(cellOf(panel, ".band1Type") == std::pair{1, Control::Selector});
    CHECK(cellOf(panel, ".band2Type") == std::pair{2, Control::Selector});
    e.world.slots.selectEffect(fx::Slot::Fx1, 1, 5, "lofi");
    e.f.frame();
    CHECK(cellOf(panel, ".filterPosition") == std::pair{2, Control::Segmented});
    e.f.frame();
    // UID-828: the bracket runs from the PRE | POST cell down to knobs 4 and 5.
    bool down4 = false, down5 = false;
    for (const auto& c : e.f.dl.commands())
        if (c.op == ui::DrawCmd::Op::Fill && c.rect.y == 328 && c.rect.h == 180)
        {
            down4 |= c.rect.x + 0.5f == ui::EffectPanel::knobX(3);
            down5 |= c.rect.x + 0.5f == ui::EffectPanel::knobX(4);
        }
    CHECK(down4);
    CHECK(down5);
    // Bus compressor: LOOK AHEAD in cell 1.
    e.world.slots.selectEffect(fx::Slot::Master, 1, 0, "compressor");
    auto& bus = e.open("mixer.busFx", {}, fx::Slot::Master);
    CHECK(cellOf(bus, ".lookAhead") == std::pair{1, Control::Toggle});
}

TEST_CASE("fxui.panel_geometry: cells, knob glyphs and header sit on the effects.md table (UID-825)", "[fxui]")
{
    namespace L = ui::layout;
    for (int c = 0; c < 6; ++c)
    {
        const auto r = ui::EffectPanel::cell(c);
        CHECK(r.x == 24 + 208 * c);
        CHECK(r.w == 192);
        CHECK(L::onGrid(r));
        CHECK(L::insideTemplate(ui::BodyTemplate::Full, r));
        CHECK(ui::EffectPanel::knobX(c) == 120 + 208 * c);
        CHECK(ui::EffectPanel::knobX(c) == L::columnX(c) + L::columnW / 2); // glyph n over strip column n
    }
    for (const auto r : {ui::EffectPanel::kSelectFx, ui::EffectPanel::kBypass, ui::EffectPanel::kBusPage,
                         ui::EffectPanel::kMixHint, ui::fxzone::zone})
    {
        CHECK(L::onGrid(r));
        CHECK(L::insideTemplate(ui::BodyTemplate::Full, r));
    }
    // The glyph row never moves between effects (UID-003): the arc tracks of two different effects coincide.
    Env e;
    auto& panel = e.open("track.fx1", "chorus");
    auto rings = [&]
    {
        std::vector<float> xs;
        for (const auto& c : e.f.frame().commands())
            if (c.op == ui::DrawCmd::Op::Stroke && c.rect.w == 66 && c.rect.h == 66)
                xs.push_back(c.rect.x + 33);
        return xs;
    };
    CHECK(rings() == std::vector<float>{120, 328, 536, 744, 1160}); // CHORUS: position 5 is empty
    e.world.slots.selectEffect(fx::Slot::Fx1, 1, 5, "flanger");
    (void)panel;
    CHECK(rings() == std::vector<float>{120, 328, 536, 744, 952, 1160});
}

TEST_CASE("fxui.panel_sync: SYNC swaps the rate column to DIV, the Ducker keeps its captions (UID-830)", "[fxui]")
{
    Env e;
    auto& panel = e.open("track.fx1", "chorus");
    CHECK(e.f.shell->boundColumn(0)->label == "RATE");
    e.f.tap(120, 280); // the SYNC toggle in cell 1
    e.f.frame();
    CHECK(panel.syncOn());
    CHECK(e.f.get("fx1.chorus.sync") == 1);
    CHECK(e.f.shell->boundColumn(0) == &e.world.spec("fx1.chorus.rate.div"));
    CHECK(e.f.shell->boundColumn(0)->label == "DIV");
    e.f.frame();
    CHECK(e.f.drewText("DIV"));
    // The default division: the fixture's enum value "8" (index 1), or — once the real Chorus catalogue is registered —
    // the text of its continuous `.div` at the default rate.
    const auto& div = e.world.spec("fx1.chorus.rate.div");
    char divText[32] = "8";
    if (div.format != nullptr)
        div.format(e.f.get("fx1.chorus.rate.div"), divText, static_cast<int>(sizeof divText));
    CHECK(e.f.drewText(divText));
    e.f.tap(120, 280);
    e.f.frame();
    CHECK(e.f.shell->boundColumn(0) == &e.world.spec("fx1.chorus.rate"));

    e.world.slots.selectEffect(fx::Slot::Fx1, 1, 5, "ducker");
    e.f.set("fx1.ducker.sync", 1);
    e.f.frame();
    e.f.frame();
    CHECK(e.f.shell->boundColumn(0) == &e.world.spec("fx1.ducker.attack.div"));
    CHECK(e.f.shell->boundColumn(2) == &e.world.spec("fx1.ducker.release.div"));
    CHECK(e.f.shell->boundColumn(0)->label == "ATTACK");
    CHECK(e.f.shell->boundColumn(2)->label == "RELEASE");
}

TEST_CASE("fxui.panel_mix_hint: MIX RATIO is 0 shows only while a default-0 mix is 0 (UID-837, FX-093)", "[fxui]")
{
    Env e;
    auto& panel = e.open("track.fx1", "chorus");
    CHECK(panel.mixHint());
    e.f.frame();
    CHECK(e.f.drewText("MIX RATIO is 0"));
    e.f.turn(io::knob(6), 10);
    e.f.frame();
    CHECK_FALSE(panel.mixHint());
    CHECK_FALSE(e.f.drewText("MIX RATIO is 0"));
    e.world.slots.selectEffect(fx::Slot::Fx1, 1, 5, "filter"); // default mix 1.0: never a hint
    e.f.set("fx1.filter.mix", 0);
    e.f.frame();
    CHECK_FALSE(panel.mixHint());
}

TEST_CASE("fxui.panel_none: NONE shows the header, the hint text and an empty strip with its tab (UID-841)", "[fxui]")
{
    Env e;
    auto& panel = e.open("track.fx1");
    CHECK(panel.effect()->id == "none");
    e.f.frame();
    CHECK(e.f.drewText("NONE"));
    CHECK(e.f.drewText("No effect in this slot. SELECT FX to choose one."));
    CHECK(e.f.drewText("SELECT FX"));
    CHECK(e.f.drewText("FX1 \xC2\xB7 INSERT"));
    CHECK(e.f.shell->stripTab() == "FX1");
    CHECK(panel.visual() == nullptr);
}

TEST_CASE("fxui.panel_bus_page: MASTER | SEND pages, parity modes and the inactive banner (UID-839, UI-267)", "[fxui]")
{
    Env e;
    e.world.slots.selectEffect(fx::Slot::Send, 1, 0, "reverb");
    e.world.slots.selectEffect(fx::Slot::Master, 1, 0, "compressor");
    auto& panel = e.open("mixer.busFx", {}, fx::Slot::Send);
    e.f.frame();
    CHECK(e.f.drewText("SEND FX \xC2\xB7 FX MODE BOTH"));
    // BOTH: the segment only switches page.
    e.f.tap(700, 188); // MASTER
    e.f.frame();
    CHECK(panel.slot() == fx::Slot::Master);
    CHECK(panel.effect()->id == "compressor");
    CHECK(e.world.slots.mode(1) == fx::FxMode::Both);
    CHECK(e.f.shell->stripTab() == "MASTER FX");
    // A parity mode set elsewhere: the page on show (MASTER) is disabled in mode SEND → caution banner + USE MASTER.
    e.world.slots.setMode(1, fx::FxMode::Send);
    e.f.frame();
    CHECK(e.f.drewText("MASTER FX is inactive in FX mode SEND"));
    CHECK(e.f.drewText("USE MASTER"));
    CHECK(e.f.drewText("MASTER FX \xC2\xB7 FX MODE SEND"));
    e.f.tap(1128, 280); // USE MASTER
    e.f.frame();
    CHECK(e.world.slots.mode(1) == fx::FxMode::Master);
    CHECK_FALSE(e.f.drewText("USE MASTER"));
    // In a parity mode a tap on a segment also sets the mode (the reference's single control).
    e.f.tap(820, 188); // SEND
    e.f.frame();
    CHECK(panel.slot() == fx::Slot::Send);
    CHECK(e.world.slots.mode(1) == fx::FxMode::Send);
}

TEST_CASE("fxui.panel_bypass: BYPASS toggles the slot, arcs and visual drop to 40 % (UID-838)", "[fxui]")
{
    Env e;
    auto& panel = e.open("track.fx1", "filter");
    e.f.tap(968, 188);
    e.f.frame();
    CHECK(e.world.slots.bypass(fx::Slot::Fx1, 1, 5));
    REQUIRE(panel.visual() != nullptr);
    CHECK(panel.visual()->model().opacity == 0.4f);
    e.f.tap(968, 188);
    e.f.frame();
    CHECK_FALSE(e.world.slots.bypass(fx::Slot::Fx1, 1, 5));
    CHECK(panel.visual()->model().opacity == 1.0f);
}

TEST_CASE("fxui.panel_focus: rotary/keyboard reach every control in order and push activates it", "[fxui]")
{
    Env e;
    auto& panel = e.open("track.fx1", "filter");
    using T = ui::EffectPanel::Target;
    CHECK(panel.focusCount() == 4); // SELECT FX, BYPASS, TYPE, SLOPE
    e.f.turn(io::Control::Selector, 1);
    CHECK(panel.focusTarget() == T::SelectFx);
    e.f.turn(io::Control::Selector, 1);
    CHECK(panel.focusTarget() == T::Bypass);
    e.f.press(io::Control::SelectorPush);
    CHECK(e.world.slots.bypass(fx::Slot::Fx1, 1, 5));
    e.f.press(io::Control::SelectorPush);
    e.f.turn(io::Control::Selector, 1);
    CHECK(panel.focusTarget() == T::OptionCell);
    // TYPE: push opens the ListPopup focused on the current value; choosing writes it (UID-842).
    e.f.press(io::Control::SelectorPush);
    REQUIRE(e.list() != nullptr);
    CHECK(e.list()->focusedItem() == 0); // LPF
    e.f.turn(io::Control::Selector, 1);
    e.f.press(io::Control::SelectorPush);
    CHECK(e.f.get("fx1.filter.type") == 1); // HPF
    // SLOPE: push steps the segment.
    e.f.turn(io::Control::Selector, 1);
    e.f.press(io::Control::SelectorPush);
    CHECK(e.f.get("fx1.filter.slope") == 1);
    e.f.turn(io::Control::Selector, 5); // clamps at the last control
    CHECK(panel.focusIndex() == 3);
    // Back to SELECT FX and push opens the grid.
    e.f.turn(io::Control::Selector, -10);
    e.f.press(io::Control::SelectorPush);
    CHECK(e.grid() != nullptr);
}

TEST_CASE("fxui.screen_registry: the effect screens validate (parents, body component, strip ids)", "[fxui]")
{
    Env e;
    const auto& reg = ui::ScreenRegistry::builtin();
    for (const auto id : {"track.fx1", "track.fx2", "mixer.busFx"})
    {
        const auto* s = reg.find(id);
        REQUIRE(s != nullptr);
        CHECK(s->bodyComponent == "EffectPanel");
        CHECK(s->body == ui::BodyTemplate::Full);
    }
    CHECK(reg.find("track.fx1")->contextBar == ui::ContextBarKind::TrackContext);
    CHECK(reg.find("mixer.busFx")->contextBar == ui::ContextBarKind::Menu);
    CHECK(ui::PopupRegistry::builtin().find("fx.select") != nullptr);
    // Validate the three specs in a registry with the parents they name (the shell's fallbacks provide them).
    std::vector<std::string> problems =
        e.f.screens.validate(ui::WidgetRegistry::builtin().dispatcher(), e.world.params);
    for (const auto& p : problems)
        if (p.starts_with("track.fx") || p.starts_with("mixer.busFx"))
            FAIL(p);
    // Breadcrumbs: HOME › TRACK MENU › FX1 and HOME › MIXER › MASTER/SEND FX.
    REQUIRE(e.f.shell->open("mixer.busFx"));
    const auto crumb = e.f.shell->router().breadcrumb();
    CHECK(std::vector<std::string_view>(crumb.begin(), crumb.end()) ==
          std::vector<std::string_view>{"HOME", "MIXER", "MASTER/SEND FX"});
}
