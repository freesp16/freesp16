// The effect screens over the document model and the engine: what the composition root installs in place of
// LocalFxSlots and the mirror meters.
//
//   ModelFxSlots      FxSlotAccess over the CommandBus: the slot's type, bypass and the scene's FX mode are the model's
//                     (core::model::EffectSlot / SceneRouting::fxMode) and every edit is an undoable FX command
//                     (core/commands/fx: SelectFxType, makeSetFxBypass, makeSetFxMode). Effects the model has no
//                     EffectType for (the framework's test effects behind fx.test, P3 extensions until the model
//                     grows them) are kept in a LocalFxSlots and posted to the engine directly (our choice: not saved,
//                     not undoable).
//   forwardFxChanges  the CommandBus bridge hook's FX half: every FX slot / FX mode path of a ChangeSet → the slot's
//                     ParamChanges (`<slot>.<effect>.<key>` knobs and options, `<slot>.bypass`, `<slot>.type` as
//                     the engine's type index + 1 — engine::fx::engineTypes — and `fx.mode`).
//   EngineFxMeters    FxEnvironment::meters from the engine's FxMeterFeed (EngineBridge::fxMeters):
//                     asks the engine to meter the slot on show (`fx.meteredSlot`) and converts the feed to the
//                     panel's FxMeterView.
//
// Wiring (message thread, after the bus holds a project; see app/Document.cpp):
//     ui::fx::ModelFxSlots fxSlots(bus, &engine->bridge());
//     ui::fx::EngineFxMeters fxMeters(engine->bridge());
//     ui::fx::environment().slots = &fxSlots;  fxMeters.install(ui::fx::environment());
//     bridge hook:  ui::fx::forwardFxChanges(changes, project, engine->bridge());
//     project open: ui::fx::forwardAllFx(project, engine->bridge());
#pragma once

#include "core/commands/Command.h"
#include "core/params/ParamRegistry.h"
#include "engine/EngineApi.h"
#include "engine/fx/FxSlotState.h"
#include "ui/screens/effects/FxSlots.h"

#include <cstdint>
#include <string_view>

namespace core::model
{
struct Project;
}

namespace ui::fx
{
class ModelFxSlots final : public FxSlotAccess
{
public:
    // `bridge` (optional) receives the non-model effects' slot state; `params` resolves their ParamSpec ids.
    explicit ModelFxSlots(core::CommandBus&, engine::EngineBridge* bridge = nullptr,
                          const core::ParamRegistry& params = core::builtinParams(),
                          const engine::fx::EffectRegistry& effects = engine::fx::EffectRegistry::builtin());
    std::string_view effect(Slot, int scene, int track) const override;
    void selectEffect(Slot, int scene, int track, std::string_view effectId) override;
    bool bypass(Slot, int scene, int track) const override;
    void setBypass(Slot, int scene, int track, bool on) override;
    FxMode mode(int scene) const override;
    void setMode(int scene, FxMode) override;
    std::uint32_t changes() const noexcept override { return changes_ + local_.changes(); }

private:
    void postLocal(Slot, int scene, int track, std::string_view effectId);

    core::CommandBus& bus_;
    engine::EngineBridge* bridge_;
    const core::ParamRegistry& params_;
    const engine::fx::EffectRegistry& effects_;
    LocalFxSlots local_;
    core::Subscription observed_;
    std::uint32_t changes_ = 0;
};

void forwardFxChanges(const core::ChangeSet&, const core::model::Project&, engine::EngineBridge&,
                      const core::ParamRegistry& params = core::builtinParams());
// Every FX slot and the FX mode of the current scene, Off slots included (a project was opened or replaced).
// `scene` ≥ 0: that scene instead of the current one (the engine keeps every scene's FX values).
void forwardAllFx(const core::model::Project&, engine::EngineBridge&,
                  const core::ParamRegistry& params = core::builtinParams(), int scene = -1);

class EngineFxMeters
{
public:
    explicit EngineFxMeters(engine::EngineBridge&, const core::ParamRegistry& params = core::builtinParams());
    void install(FxEnvironment& env) noexcept
    {
        env.meters = &EngineFxMeters::source;
        env.metersUser = this;
    }
    // The engine slot index of (slot, track 1–16): FX1 2(t−1), FX2 2(t−1)+1, Send 32, Master 33.
    static int slotIndex(Slot, int track) noexcept;
    // The FxMeterView of `slot` from one feed (also used by tests): idle values while another slot is metered.
    static void convert(const engine::fx::FxMeterFeed&, int slotIndex, double nowMs, FxMeterView& out) noexcept;
    // The FxMeterSource (`self` = this): the slot's view, or null without an engine feed.
    static const FxMeterView* source(Slot, int track, void* self);

private:
    engine::EngineBridge& bridge_;
    core::ParamId meterId_{};
    int requested_ = -2;
    FxMeterView view_{};
};
} // namespace ui::fx
