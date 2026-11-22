// SELECT FX (screens/effects.md "SELECT FX"; UID-831…833, UI-262, FX-011, OQ-08): the grid is generated from the
// registry in the reference order, opens on the current effect, selects with one tap or turn + push, cancels with
// BACK, pages beyond 15 entries and lists flagged extensions only with their flag (EXT badge).
#include "tests/unit/fxui/FxuiTest.h"
#include "ui/widgets/FxGrid.h"

#include <vector>

using namespace fxuitest;
namespace fx = ui::fx;
namespace efx = engine::fx;

namespace
{
std::vector<std::string_view> labels(const ui::GridRequest& r)
{
    std::vector<std::string_view> out;
    for (const auto& it : r.items)
        out.push_back(it.label);
    return out;
}
std::vector<std::string_view> ids(efx::SlotKind k, const ui::FeatureFlags& flags = {})
{
    std::vector<const efx::EffectDescriptor*> list;
    fx::listFor(fxtest::World::get().effects, k, flags, list);
    std::vector<std::string_view> out;
    for (const auto* d : list)
        out.push_back(d->id);
    return out;
}
} // namespace

TEST_CASE("fxui.select_fx_order: insert and bus grids follow UID-831 with the registry's descriptors", "[fxui]")
{
    Env e;
    using V = std::vector<std::string_view>;
    CHECK(labels(ui::FxGrid::request(fx::Slot::Fx1, 5, "none", {})) == V{"NONE", "CHORUS", "FLANGER", "2 BAND EQ",
                                                                         "FILTER", "DELAY", "LO-FI", "COMPRESSOR",
                                                                         "DISTORTION", "PHASER", "DUCKER"});
    CHECK(labels(ui::FxGrid::request(fx::Slot::Send, 0, "none", {})) == V{"NONE", "DELAY", "REVERB", "CHORUS",
                                                                          "FLANGER", "2 BAND EQ", "FILTER", "LO-FI",
                                                                          "COMPRESSOR", "DISTORTION", "PHASER"});
    CHECK(ids(efx::SlotKind::Master) == ids(efx::SlotKind::Send));
    // Effects a slot cannot run are not listed (UID-831): the insert delay is insert-only, the bus delay bus-only.
    CHECK(ids(efx::SlotKind::Insert)[5] == "delay");
    CHECK(ids(efx::SlotKind::Send)[1] == "delayBus");
}

TEST_CASE("fxui.select_fx_flags: extensions appear only with their flag, after the reference cells, with EXT", "[fxui]")
{
    Env e;
    ui::FeatureFlags none;
    for (const auto id : ids(efx::SlotKind::Insert, none))
        CHECK(id != "tremolo");
    ui::FeatureFlags ext;
    ext.enable(fx::kExtensionsFlag);
    const auto withExt = ids(efx::SlotKind::Insert, ext);
    REQUIRE(withExt.size() == 16); // 11 reference cells + LIMITER, RING MOD, TREMOLO, AUTO PAN, AUTO WAH
    CHECK(std::vector<std::string_view>(withExt.begin() + 11, withExt.end()) ==
          std::vector<std::string_view>{"limiter", "ringmod", "tremolo", "autopan", "autowah"});
    CHECK(fx::isExtension(*fxtest::World::get().effects.find("tremolo")));
    // TREMOLO is insert-only: not on the bus even with the flag.
    for (const auto id : ids(efx::SlotKind::Send, ext))
        CHECK(id != "tremolo");
    // GAIN TEST appears only with its own flag (emulator smoke: flag fx.test), never with the extensions flag.
    for (const auto id : withExt)
        CHECK(id != "gainTest");
    ui::FeatureFlags test;
    test.enable("fx.test");
    CHECK(ids(efx::SlotKind::Insert, test).back() == "gainTest");
}

TEST_CASE("fxui.select_fx_paging: a 16th entry pages the grid, turning crosses the page", "[fxui]")
{
    Env e(true);
    auto& panel = e.open("track.fx1");
    panel.openSelectFx();
    e.f.frame();
    auto* g = e.grid();
    REQUIRE(g != nullptr);
    CHECK(g->pages() == 2);
    CHECK(g->page() == 0);
    e.f.turn(io::Control::Selector, 15);
    CHECK(g->page() == 1);
    CHECK(g->focusedItem() == 15);
    e.f.frame();
    CHECK(e.f.drewText("2 / 2")); // the pager count FxGrid draws
    CHECK(e.f.drewText("AUTO WAH"));
}

TEST_CASE("fxui.select_fx_choose: opens on the current effect, tap selects and closes, BACK cancels (UID-832)",
          "[fxui]")
{
    Env e;
    auto& panel = e.open("track.fx1", "chorus");
    CHECK(panel.effect()->id == "chorus");
    e.f.tap(1152, 188); // SELECT FX
    e.f.frame();
    auto* g = e.grid();
    REQUIRE(g != nullptr);
    CHECK(g->focusedItem() == 1); // CHORUS, the current effect
    CHECK(e.f.drewText("FX1 \xC2\xB7 TRACK 5"));
    // BACK cancels without change.
    e.f.press(io::Control::Back);
    e.f.frame();
    CHECK(e.f.shell->topPopup() == nullptr);
    CHECK(e.world.slots.effect(fx::Slot::Fx1, 1, 5) == "chorus");
    // Turn + push selects FILTER.
    panel.openSelectFx();
    e.f.turn(io::Control::Selector, 3);
    e.f.press(io::Control::SelectorPush);
    e.f.frame();
    CHECK(e.f.shell->topPopup() == nullptr);
    CHECK(e.world.slots.effect(fx::Slot::Fx1, 1, 5) == "filter");
    CHECK(panel.effect()->id == "filter");
    CHECK(e.f.shell->boundColumn(0) == &e.world.spec("fx1.filter.cutoff"));
    // One tap on a cell selects and closes (UI-262, 1.60): DELAY is row 2, column 1.
    panel.openSelectFx();
    e.f.frame();
    e.f.tap(72 + 104, 304 + 76);
    e.f.frame();
    CHECK(e.world.slots.effect(fx::Slot::Fx1, 1, 5) == "delay");
    CHECK(e.f.shell->stripTab() == "FX1");
}

TEST_CASE("fxui.select_fx_keyboard: arrows move in 2-D, Return selects, Esc cancels", "[fxui]")
{
    Env e;
    auto& panel = e.open("track.fx1");
    panel.openSelectFx();
    CHECK(e.f.shell->textKey(ui::TextKey::Down));  // NONE → DELAY
    CHECK(e.f.shell->textKey(ui::TextKey::Right)); // → LO-FI
    CHECK(e.grid()->focusedItem() == 6);
    CHECK(e.f.shell->textKey(ui::TextKey::Escape));
    CHECK(e.world.slots.effect(fx::Slot::Fx1, 1, 5) == "none");
    panel.openSelectFx();
    e.f.shell->textKey(ui::TextKey::Right);
    e.f.shell->textKey(ui::TextKey::Return);
    CHECK(e.world.slots.effect(fx::Slot::Fx1, 1, 5) == "chorus");
}

TEST_CASE("fxui.select_fx_registry: a newly registered effect takes the next cell with no change here", "[fxui]")
{
    // A private registry with one reference effect and one unknown id: the unknown one is appended (id order) and the
    // ⊘ NONE cell is synthesised when no "none" descriptor exists.
    efx::EffectRegistry reg;
    reg.add({.id = "zeta",
             .name = "ZETA",
             .slots = static_cast<std::uint8_t>(efx::SlotKind::Insert),
             .params = {},
             .options = {},
             .visual = "none",
             .shell = {},
             .create = nullptr});
    reg.add({.id = "chorus",
             .name = "CHORUS",
             .slots = fxtest::kAll,
             .params = {},
             .options = {},
             .visual = "lfoCurve",
             .shell = {},
             .create = nullptr});
    reg.add({.id = "alpha",
             .name = "ALPHA",
             .slots = fxtest::kAll,
             .params = {},
             .options = {},
             .visual = "none",
             .shell = {},
             .create = nullptr});
    std::vector<const efx::EffectDescriptor*> list;
    fx::listFor(reg, efx::SlotKind::Insert, {}, list);
    REQUIRE(list.size() == 4);
    CHECK(list[0]->id == "none");
    CHECK(list[1]->id == "chorus");
    CHECK(list[2]->id == "alpha");
    CHECK(list[3]->id == "zeta");
    fx::listFor(reg, efx::SlotKind::Send, {}, list);
    CHECK(list.size() == 3); // zeta is insert-only
}
