// The effect screens over the FX framework and the model: SELECT FX membership from engine::fx::listFor and
// the built-in NONE, ModelFxSlots (FX commands, undo, non-model test effects), the bridge hook forwardFxChanges, the
// engine's FxMeterFeed as the panels' FxMeterView, and the FX-068 value texts through the FX framework's registration
// (including the `<id>.div` SYNC form).
#include "core/commands/CommandBusAccess.h"
#include "core/model/Project.h"
#include "core/params/catalogue/fx.h"
#include "engine/fx/EffectRegistryOrder.h"
#include "tests/fakes/FakeEngineBridge.h"
#include "tests/unit/engine/EngineTestUtil.h"
#include "ui/screens/effects/FxCatalogue.h"
#include "ui/screens/effects/ModelFxSlots.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <variant>

namespace
{
namespace cmd = core::commands;
namespace fx = ui::fx;
namespace efx = engine::fx;

struct World
{
    core::CommandBus bus;
    fakes::FakeEngineBridge bridge;
    World()
    {
        cmd::replaceProject(bus, std::make_unique<core::model::Project>());
        cmd::setBridge(bus, [this](const core::ChangeSet& c, const core::model::Project& p) {
            fx::forwardFxChanges(c, p, bridge);
        });
    }
    float last(std::string_view id, std::uint16_t scope)
    {
        const auto pid = core::builtinParams().idOf(id).value_or(core::ParamId{});
        float v = -999.0f;
        for (const auto& m : bridge.posted)
            if (const auto* c = std::get_if<engine::ParamChange>(&m); c != nullptr && c->param == pid && c->scope == scope)
                v = c->value;
        return v;
    }
};

float engineType(efx::SlotKind k, std::string_view id)
{
    return static_cast<float>(efx::typeIndexOf(efx::engineTypes(efx::EffectRegistry::builtin(), k), id) + 1);
}
} // namespace

TEST_CASE("fxui.model_listFor: built-in registry: NONE first, the framework's test effects only with their flag",
          "[fxui]")
{
    std::vector<const efx::EffectDescriptor*> list;
    fx::listFor(efx::EffectRegistry::builtin(), efx::SlotKind::Insert, {}, list);
    REQUIRE_FALSE(list.empty());
    CHECK(list[0]->id == "none");
    CHECK(list[0]->name == "NONE");
    for (const auto& p : list[0]->params) // the UI's NONE, not the framework's pass-through test effect "none" (MIX)
        CHECK(p.empty());
    for (const auto* d : list)
        CHECK(d->id != "gainTest");
    ui::FeatureFlags flags;
    flags.enable(efx::kTestEffectFlag);
    fx::listFor(efx::EffectRegistry::builtin(), efx::SlotKind::Master, flags, list);
    CHECK(list[0]->name == "NONE");
    CHECK(std::count_if(list.begin(), list.end(), [](const auto* d) { return d->id == "gainTest"; }) == 1);
    CHECK(std::count_if(list.begin(), list.end(), [](const auto* d) { return d->id == "none"; }) == 1);
    CHECK(fx::findEffect(efx::EffectRegistry::builtin(), "")->name == "NONE");
}

TEST_CASE("fxui.model_slots: SELECT FX, bypass and FX mode are undoable model edits (FX-006/044/008)", "[fxui]")
{
    World w;
    fx::ModelFxSlots slots(w.bus, &w.bridge);
    CHECK(slots.effect(fx::Slot::Fx1, 1, 5) == "none");
    slots.selectEffect(fx::Slot::Fx1, 1, 5, "chorus");
    CHECK(cmd::project(w.bus).scenes[0].tracks[4].insertFx[0].type == core::model::EffectType::Chorus);
    CHECK(slots.effect(fx::Slot::Fx1, 1, 5) == "chorus");
    CHECK(w.last("fx1.type", 4) == engineType(efx::SlotKind::Insert, "chorus")); // the bridge hook (0: not registered)
    slots.setBypass(fx::Slot::Fx1, 1, 5, true);
    CHECK(slots.bypass(fx::Slot::Fx1, 1, 5));
    CHECK(w.last("fx1.bypass", 4) == 1.0f);
    slots.setMode(3, fx::FxMode::Send);
    CHECK(slots.mode(3) == fx::FxMode::Send);
    CHECK(w.last("fx.mode", 2) == 1.0f);
    const auto before = slots.changes();
    REQUIRE(w.bus.undo());
    REQUIRE(w.bus.undo());
    REQUIRE(w.bus.undo());
    cmd::flushNotifications(w.bus);
    CHECK(slots.changes() != before); // undo rebinds the panel
    CHECK(slots.effect(fx::Slot::Fx1, 1, 5) == "none");
    CHECK_FALSE(slots.bypass(fx::Slot::Fx1, 1, 5));
    CHECK(slots.mode(3) == fx::FxMode::Both);
    CHECK(w.last("fx1.type", 4) == 0.0f);
}

TEST_CASE("fxui.model_slots: the Send / Master bypass is an undoable model edit sent to the engine (FX-046)", "[fxui]")
{
    World w;
    fx::ModelFxSlots slots(w.bus, &w.bridge);
    slots.setBypass(fx::Slot::Master, 3, 0, true);
    slots.setBypass(fx::Slot::Send, 3, 0, true);
    const auto& scene = cmd::project(w.bus).scenes[2];
    CHECK(scene.masterFx.bypass); // the model's (saved with the project)
    CHECK(scene.sendFx.bypass);
    CHECK(slots.bypass(fx::Slot::Master, 3, 0));
    CHECK(slots.bypass(fx::Slot::Send, 3, 0));
    CHECK_FALSE(slots.bypass(fx::Slot::Master, 1, 0));
    CHECK(w.last("master.bypass", 2) == 1.0f); // the bridge hook, scope = the scene
    CHECK(w.last("send.bypass", 2) == 1.0f);
    REQUIRE(w.bus.undo());
    cmd::flushNotifications(w.bus);
    CHECK_FALSE(cmd::project(w.bus).scenes[2].sendFx.bypass);
    CHECK(cmd::project(w.bus).scenes[2].masterFx.bypass);
    CHECK(w.last("send.bypass", 2) == 0.0f);
    // A project open sends every slot's bypass
    w.bridge.posted.clear();
    fx::forwardAllFx(cmd::project(w.bus), w.bridge, core::builtinParams(), 2);
    CHECK(w.last("master.bypass", 2) == 1.0f);
    CHECK(w.last("send.bypass", 2) == 0.0f);
}

TEST_CASE("fxui.model_slots: an effect without a model type is kept locally and posted to the engine", "[fxui]")
{
    World w;
    fx::ModelFxSlots slots(w.bus, &w.bridge);
    slots.selectEffect(fx::Slot::Master, 1, 0, "gainTest");
    CHECK(slots.effect(fx::Slot::Master, 1, 0) == "gainTest");
    CHECK(cmd::project(w.bus).scenes[0].masterFx.type == core::model::EffectType::Off);
    CHECK(w.last("master.gainTest.gain", 0) == core::builtinParams().find("master.gainTest.gain")->defaultValue);
    CHECK(w.last("master.type", 0) == engineType(efx::SlotKind::Master, "gainTest"));
    slots.selectEffect(fx::Slot::Master, 1, 0, "none");
    CHECK(slots.effect(fx::Slot::Master, 1, 0) == "none");
    CHECK(w.last("master.type", 0) == 0.0f);
}

TEST_CASE("fxui.model_engine: ModelFxSlots drives the engine's bank end to end", "[fxui]")
{
    core::CommandBus bus;
    cmd::replaceProject(bus, std::make_unique<core::model::Project>());
    auto e = std::make_unique<engine::Engine>(enginetest::fullSetup(), engine::EngineOptions{});
    e->prepare(44100.0, 512);
    cmd::setBridge(bus, [&](const core::ChangeSet& c, const core::model::Project& p) {
        fx::forwardFxChanges(c, p, e->bridge());
    });
    fx::forwardAllFx(cmd::project(bus), e->bridge());
    fx::ModelFxSlots slots(bus, &e->bridge());
    fx::EngineFxMeters meters(e->bridge());
    slots.selectEffect(fx::Slot::Master, 1, 0, "gainTest");
    CHECK(meters.source(fx::Slot::Master, 0, &meters) != nullptr); // asks the engine to meter the Master slot
    enginetest::render(*e, 4096);
    CHECK_FALSE(e->fxBank().masterIdle());
    const auto* view = meters.source(fx::Slot::Master, 0, &meters);
    REQUIRE(view != nullptr);
    CHECK(view->in[0] > 0.1f); // the test tone through GAIN TEST at unity
    CHECK(view->out[0] > 0.1f);
    const auto* other = meters.source(fx::Slot::Fx1, 3, &meters); // another panel: idle until metered
    REQUIRE(other != nullptr);
    CHECK(other->in[0] == 0.0f);
    slots.selectEffect(fx::Slot::Master, 1, 0, "none");
    enginetest::render(*e, 8192);
    CHECK(e->fxBank().masterIdle());
}

TEST_CASE("fxui.model_meters: the panels' view of an FxMeterFeed (amendment A2)", "[fxui]")
{
    efx::FxMeterFeed f{};
    f.meteredSlot = static_cast<std::int8_t>(fx::EngineFxMeters::slotIndex(fx::Slot::Fx2, 3));
    CHECK(f.meteredSlot == 5);
    f.inPeak = 0.5f;
    f.outPeak = 0.25f;
    f.telemetry[0] = 0.75f;
    f.triggerFlash = 1;
    fx::FxMeterView v;
    fx::EngineFxMeters::convert(f, 5, 1234.0, v);
    CHECK(v.in[1] == 0.5f);
    CHECK(v.out[0] == 0.25f);
    CHECK(v.gainReduction == 0.75f);
    CHECK(v.duckEnvelope == 0.75f);
    CHECK(v.lastTriggerMs == 1234.0);
    f.triggerFlash = 0;
    fx::EngineFxMeters::convert(f, 32, 2000.0, v); // not the metered slot: idle, the last flash kept
    CHECK(v.in[0] == 0.0f);
    CHECK(v.lastTriggerMs == 1234.0);
    CHECK(fx::EngineFxMeters::slotIndex(fx::Slot::Send, 1) == efx::kSendSlot);
    CHECK(fx::EngineFxMeters::slotIndex(fx::Slot::Master, 1) == efx::kMasterSlot);
}

TEST_CASE("fxui.format: FX-068 value texts reach the panel through the framework's registration and the SYNC form",
          "[fxui]")
{
    using namespace core::fx;
    static constexpr EffectParamDef kDefs[] = {
        {.key = "rate", .label = "RATE", .shortLabel = "RATE", .k = 0, .free = {1, 101}, .sync = {0, 10},
         .family = Family::Chorus},
        {.key = "time", .label = "TIME", .shortLabel = "TIME", .k = 1, .free = {10, 4001}, .sync = {0, 8},
         .family = Family::SendDelay},
        {.key = "attack", .label = "ATTACK", .shortLabel = "ATTACK", .k = 2, .free = {0, 10001}, .sync = {0, 128},
         .family = Family::DuckerAttack},
        {.key = "release", .label = "RELEASE", .shortLabel = "RELEASE", .k = 3, .free = {1, 80001},
         .sync = {0, 128}, .family = Family::DuckerRelease},
        {.key = "rateF", .label = "RATE", .shortLabel = "RATE", .k = 4, .free = {1, 101}, .sync = {0, 7},
         .family = Family::Flanger}};
    static constexpr EffectOptionDef kOpts[] = {{.key = "sync", .label = "SYNC", .shortLabel = "SYNC"}};
    static constexpr std::string_view kOptionIds[] = {"fx.fx068.sync"};
    core::ParamRegistry r;
    registerEffect(r, "fx068", kDefs, kSlotInsert | kSlotSend, {}, kOpts);
    REQUIRE(r.errors().empty());
    const efx::EffectDescriptor d{.id = "fx068",
                                  .name = "FX068",
                                  .slots = static_cast<std::uint8_t>(efx::SlotKind::Insert | efx::SlotKind::Send),
                                  .params = {"fx.fx068.rate", "fx.fx068.time", "fx.fx068.attack", "fx.fx068.release",
                                             "fx.fx068.rateF", ""},
                                  .options = kOptionIds,
                                  .visual = {},
                                  .shell = {},
                                  .create = nullptr};
    char b[32];
    auto text = [&](const core::ParamSpec* s, float v) {
        REQUIRE(s != nullptr);
        s->formatValue(v, b, sizeof b);
        return std::string(b);
    };
    const auto* rate = fx::resolveParam(r, fx::Slot::Fx1, d, d.params[0]);
    CHECK(text(rate, valueOfStep(15, {1, 101})) == "1.5 Hz");
    CHECK(text(fx::resolveParam(r, fx::Slot::Fx1, d, d.params[4]), valueOfStep(15, {1, 101})) == "0.15 Hz");
    CHECK(text(fx::resolveParam(r, fx::Slot::Send, d, d.params[1]), valueOfStep(250, {10, 4001})) == "250 ms");
    CHECK(text(fx::resolveParam(r, fx::Slot::Fx1, d, d.params[3]), valueOfStep(125, {1, 80001})) == "12.5 ms");
    REQUIRE(fx::syncOption(r, fx::Slot::Fx1, d) != nullptr);
    // SYNC on: the panel binds the `<id>.div` sibling (UID-830): DIV and the division text; the Ducker keeps its
    // caption and shows sixteenths.
    const auto* div = fx::syncForm(r, *rate);
    REQUIRE(div != nullptr);
    CHECK(div->label == "DIV");
    CHECK(text(div, valueOfStep(5, {0, 10})) == "3/4");
    const auto* attackDiv = fx::syncForm(r, *fx::resolveParam(r, fx::Slot::Fx1, d, d.params[2]));
    REQUIRE(attackDiv != nullptr);
    CHECK(attackDiv->label == "ATTACK");
    CHECK(text(attackDiv, valueOfStep(127, {0, 128})) == "4.00");
    CHECK(fx::syncForm(r, *fx::resolveParam(r, fx::Slot::Send, d, d.params[1])) != nullptr);
}
