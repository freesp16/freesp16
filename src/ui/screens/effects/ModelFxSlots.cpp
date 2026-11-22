// ModelFxSlots, forwardFxChanges, EngineFxMeters — see ModelFxSlots.h. Message thread only.
#include "ui/screens/effects/ModelFxSlots.h"

#include "core/commands/CommandBusAccess.h"
#include "core/commands/fx/FxCommands.h"
#include "core/model/Project.h"
#include "core/params/catalogue/fx.h"
#include "engine/fx/EffectRegistryOrder.h"
#include "ui/screens/effects/FxCatalogue.h"

#include <array>
#include <bitset>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace ui::fx
{
namespace
{
using core::model::ParamPath;
using core::model::Scope;
namespace efx = engine::fx;

ParamPath pathOf(Slot s, int scene, int track) noexcept
{
    ParamPath p{};
    p.scene = static_cast<std::uint8_t>((scene - 1) & 15);
    switch (s)
    {
    case Slot::Fx1:
    case Slot::Fx2:
        p.scope = Scope::InsertFx;
        p.track = static_cast<std::uint8_t>((track - 1) & 15);
        p.slot = s == Slot::Fx2 ? 1 : 0;
        break;
    case Slot::Send: p.scope = Scope::SendFx; break;
    case Slot::Master: p.scope = Scope::MasterFx; break;
    }
    return p;
}

std::uint16_t scopeOf(Slot s, int scene0, int track0) noexcept
{
    return isInsert(s) ? static_cast<std::uint16_t>((scene0 & 15) << 4 | (track0 & 15))
                       : static_cast<std::uint16_t>(scene0 & 15);
}

// The engine's type lists (engine::fx::engineTypes), once: the registry is complete before main().
const std::vector<const efx::EffectDescriptor*>& engineList(efx::SlotKind k)
{
    static const std::array<std::vector<const efx::EffectDescriptor*>, 3> lists{
        efx::engineTypes(efx::EffectRegistry::builtin(), efx::SlotKind::Insert),
        efx::engineTypes(efx::EffectRegistry::builtin(), efx::SlotKind::Send),
        efx::engineTypes(efx::EffectRegistry::builtin(), efx::SlotKind::Master)};
    return lists[k == efx::SlotKind::Insert ? 0 : (k == efx::SlotKind::Send ? 1 : 2)];
}

float typeValue(Slot s, std::string_view effectId) noexcept
{
    if (isNone(effectId))
        return 0.0f;
    const int i = efx::typeIndexOf(engineList(kind(s)), effectId);
    return i < 0 ? 0.0f : static_cast<float>(i + 1);
}

void post(engine::EngineBridge& bridge, const core::ParamRegistry& params, std::string_view id, std::uint16_t scope,
          float value)
{
    if (const auto pid = params.idOf(id); pid && pid->valid())
        bridge.post(engine::ParamChange{*pid, scope, value, 0});
}

std::string idOf(Slot s, std::string_view effect, std::string_view key)
{
    std::string id(prefix(s));
    id += '.';
    id += effect;
    id += '.';
    id += key;
    return id;
}

// One slot of the model → the engine: knobs and options first (the bank keeps them per type), then bypass and type.
void postSlot(const core::model::EffectSlot& slot, Slot s, int scene0, int track0, engine::EngineBridge& bridge,
              const core::ParamRegistry& params)
{
    const auto scope = scopeOf(s, scene0, track0);
    const std::string_view effect = core::fx::effectIdOf(slot.type);
    if (const auto* set = effect.empty() ? nullptr : core::fx::effectParams(effect))
    {
        for (const auto& d : set->params)
            if (d.k >= 0 && d.k < 6)
                post(bridge, params, idOf(s, effect, d.key), scope,
                     static_cast<float>(slot.params[static_cast<std::size_t>(d.k)]));
        for (const auto& o : set->options)
        {
            float v = 0;
            if (o.key == "type1")
                v = static_cast<float>(slot.type1);
            else if (o.key == "type2")
                v = static_cast<float>(slot.type2);
            else if (o.key == "sync")
                v = slot.sync ? 1.0f : 0.0f;
            else if (o.key == "lookAhead")
                v = slot.lookAhead ? 1.0f : 0.0f;
            else
                continue; // the effect's own options are not in the model yet
            post(bridge, params, idOf(s, effect, o.key), scope, v);
        }
    }
    post(bridge, params, std::string(prefix(s)) + ".bypass", scope, slot.bypass ? 1.0f : 0.0f); // FX-045/046
    post(bridge, params, std::string(prefix(s)) + ".type", scope, typeValue(s, effect));
}

void postMode(const core::model::Project& p, int scene0, engine::EngineBridge& bridge,
              const core::ParamRegistry& params)
{
    post(bridge, params, "fx.mode", static_cast<std::uint16_t>(scene0 & 15),
         static_cast<float>(p.scenes[static_cast<std::size_t>(scene0 & 15)].routing.fxMode));
}
} // namespace

// ---------------------------------------------------------------------------------------------------- ModelFxSlots

ModelFxSlots::ModelFxSlots(core::CommandBus& bus, engine::EngineBridge* bridge, const core::ParamRegistry& params,
                           const engine::fx::EffectRegistry& effects)
    : bus_(bus), bridge_(bridge), params_(params), effects_(effects)
{
    // Undo / redo / a project open change the slots behind the panel's back: rebind on any scene change.
    observed_ = bus_.observe("scene[", [this](const core::ChangeSet&) { ++changes_; });
}

std::string_view ModelFxSlots::effect(Slot s, int scene, int track) const
{
    const std::string_view local = local_.effect(s, scene, track);
    if (! isNone(local))
        return local;
    if (! core::commands::hasProject(bus_))
        return kNone;
    const auto at = pathOf(s, scene, track); // a named path: the slot reference never looks bound to a temporary
    const auto& slot = core::fxSlotAt(core::commands::project(bus_), at);
    const std::string_view id = core::fx::effectIdOf(slot.type);
    if (id.empty())
        return kNone;
    const auto* d = effects_.find(id);
    return d != nullptr ? d->id : id; // a model type without a registered effect still names its id (literal)
}

void ModelFxSlots::selectEffect(Slot s, int scene, int track, std::string_view effectId)
{
    const auto type = isNone(effectId) ? core::model::EffectType::Off : core::fx::effectTypeOf(effectId);
    const bool modelType = isNone(effectId) || type != core::model::EffectType::Off;
    if (core::commands::hasProject(bus_))
    {
        const auto at = pathOf(s, scene, track);
        if (core::fxSlotAt(core::commands::project(bus_), at).type != type)
            bus_.execute(std::make_unique<core::SelectFxType>(at, type));
    }
    const bool hadLocal = ! isNone(local_.effect(s, scene, track));
    local_.selectEffect(s, scene, track, modelType ? kNone : effectId);
    if (! modelType || hadLocal)
        postLocal(s, scene, track, modelType ? std::string_view{} : effectId);
    ++changes_;
}

void ModelFxSlots::postLocal(Slot s, int scene, int track, std::string_view effectId)
{
    if (bridge_ == nullptr)
        return;
    const auto scope = scopeOf(s, scene - 1, track - 1);
    if (effectId.empty()) // back to a model type: the bridge hook already posted the model's slot
    {
        if (core::commands::hasProject(bus_))
            postSlot(core::fxSlotAt(core::commands::project(bus_), pathOf(s, scene, track)), s, scene - 1, track - 1,
                     *bridge_, params_);
        return;
    }
    // A non-model effect: its ParamSpec defaults, then the type (the bank keeps per-type values, FX-006).
    if (const auto* d = effects_.find(effectId))
        for (const auto pid : d->params)
            if (const auto* spec = resolveParam(params_, s, *d, pid))
                post(*bridge_, params_, spec->id, scope, spec->defaultValue);
    post(*bridge_, params_, std::string(prefix(s)) + ".type", scope, typeValue(s, effectId));
}

bool ModelFxSlots::bypass(Slot s, int scene, int track) const
{
    if (! core::commands::hasProject(bus_))
        return local_.bypass(s, scene, track);
    return core::fxSlotAt(core::commands::project(bus_), pathOf(s, scene, track)).bypass;
}

void ModelFxSlots::setBypass(Slot s, int scene, int track, bool on)
{
    if (core::commands::hasProject(bus_)) // every slot: an undoable, saved model edit (FX-045/046)
        bus_.execute(core::makeSetFxBypass(pathOf(s, scene, track), on));
    else
        local_.setBypass(s, scene, track, on);
    ++changes_;
}

FxMode ModelFxSlots::mode(int scene) const
{
    if (! core::commands::hasProject(bus_))
        return local_.mode(scene);
    return static_cast<FxMode>(
        core::commands::project(bus_).scenes[static_cast<std::size_t>((scene - 1) & 15)].routing.fxMode);
}

void ModelFxSlots::setMode(int scene, FxMode m)
{
    if (core::commands::hasProject(bus_))
        bus_.execute(core::makeSetFxMode(static_cast<std::uint8_t>((scene - 1) & 15),
                                         static_cast<core::model::FxMode>(m)));
    else
        local_.setMode(scene, m);
    ++changes_;
}

// ------------------------------------------------------------------------------------------------ the bridge hook

void forwardFxChanges(const core::ChangeSet& changes, const core::model::Project& p, engine::EngineBridge& bridge,
                      const core::ParamRegistry& params)
{
    std::bitset<16 * 16 * 2 + 16 * 2 + 16> seen; // insert slots, bus slots, modes: each posted once per ChangeSet
    for (const auto& path : changes.paths())
    {
        int s = -1, t = -1, u = -1, n = 0;
        if (std::sscanf(path.c_str(), "scene[%d].track[%d].insertFx[%d]%n", &s, &t, &u, &n) == 3 && n > 0)
        {
            if (s < 0 || s > 15 || t < 0 || t > 15 || u < 0 || u > 1)
                continue;
            const std::size_t key = static_cast<std::size_t>((s * 16 + t) * 2 + u);
            if (seen.test(key))
                continue;
            seen.set(key);
            postSlot(p.scenes[static_cast<std::size_t>(s)].tracks[static_cast<std::size_t>(t)]
                         .insertFx[static_cast<std::size_t>(u)],
                     u == 0 ? Slot::Fx1 : Slot::Fx2, s, t, bridge, params);
            continue;
        }
        n = 0;
        if (std::sscanf(path.c_str(), "scene[%d].%n", &s, &n) != 1 || n == 0 || s < 0 || s > 15)
            continue;
        const std::string_view rest = std::string_view(path).substr(static_cast<std::size_t>(n));
        const auto& scene = p.scenes[static_cast<std::size_t>(s)];
        auto once = [&](std::size_t key) {
            if (seen.test(key))
                return false;
            seen.set(key);
            return true;
        };
        if (rest.substr(0, 6) == "sendFx" && once(512 + static_cast<std::size_t>(s) * 2))
            postSlot(scene.sendFx, Slot::Send, s, 0, bridge, params);
        else if (rest.substr(0, 8) == "masterFx" && once(512 + static_cast<std::size_t>(s) * 2 + 1))
            postSlot(scene.masterFx, Slot::Master, s, 0, bridge, params);
        else if (rest == "routing.fxMode" && once(544 + static_cast<std::size_t>(s)))
            postMode(p, s, bridge, params);
    }
}

void forwardAllFx(const core::model::Project& p, engine::EngineBridge& bridge, const core::ParamRegistry& params,
                  int sceneOrCurrent)
{
    // The bank holds the current scene's FX only (other scenes' changes are ignored; a scene switch installs its
    // FxSlotState): every slot of that scene, Off ones included (the previous project's may be running).
    const int s = (sceneOrCurrent >= 0 ? sceneOrCurrent : p.settings.currentScene) & 15;
    const auto& scene = p.scenes[static_cast<std::size_t>(s)];
    for (int t = 0; t < 16; ++t)
        for (int u = 0; u < 2; ++u)
            postSlot(scene.tracks[static_cast<std::size_t>(t)].insertFx[static_cast<std::size_t>(u)],
                     u == 0 ? Slot::Fx1 : Slot::Fx2, s, t, bridge, params);
    postSlot(scene.sendFx, Slot::Send, s, 0, bridge, params);
    postSlot(scene.masterFx, Slot::Master, s, 0, bridge, params);
    postMode(p, s, bridge, params);
}

// ---------------------------------------------------------------------------------------------- EngineFxMeters

EngineFxMeters::EngineFxMeters(engine::EngineBridge& bridge, const core::ParamRegistry& params) : bridge_(bridge)
{
    meterId_ = params.idOf("fx.meteredSlot").value_or(core::ParamId{});
}

int EngineFxMeters::slotIndex(Slot s, int track) noexcept
{
    switch (s)
    {
    case Slot::Fx1: return efx::insertSlot((track - 1) & 15, 0);
    case Slot::Fx2: return efx::insertSlot((track - 1) & 15, 1);
    case Slot::Send: return efx::kSendSlot;
    case Slot::Master: return efx::kMasterSlot;
    }
    return -1;
}

void EngineFxMeters::convert(const engine::fx::FxMeterFeed& f, int slot, double nowMs, FxMeterView& out) noexcept
{
    const double lastTrigger = out.lastTriggerMs;
    out = FxMeterView{};
    out.lastTriggerMs = lastTrigger;
    if (f.meteredSlot != slot)
        return;
    out.in = {f.inPeak, f.inPeak};
    out.out = {f.outPeak, f.outPeak};
    // telemetry(0) is the effect's own reduction value (UID-834): the Compressor's GR, the Ducker's envelope; each
    // visual reads the field of its effect.
    out.gainReduction = f.telemetry[0];
    out.duckEnvelope = f.telemetry[0];
    if (f.triggerFlash != 0) // any track's trigger: the feed does not name the Ducker's SOURCE track
        out.lastTriggerMs = nowMs;
}

const FxMeterView* EngineFxMeters::source(Slot s, int track, void* self)
{
    auto& m = *static_cast<EngineFxMeters*>(self);
    const int slot = slotIndex(s, track);
    if (slot != m.requested_ && m.meterId_.valid() &&
        m.bridge_.post(engine::ParamChange{m.meterId_, 0xFFFF, static_cast<float>(slot), 0}))
        m.requested_ = slot;
    const auto* feed = m.bridge_.fxMeters();
    if (feed == nullptr)
        return nullptr;
    using namespace std::chrono;
    const double now = duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
    convert(*feed, slot, now, m.view_);
    return &m.view_;
}
} // namespace ui::fx
