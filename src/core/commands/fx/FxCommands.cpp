// FX commands — see FxCommands.h.
#include "core/commands/fx/FxCommands.h"

#include "core/commands/SetParam.h"
#include "core/model/Project.h"
#include "core/model/enums/ValueLists.h"
#include "core/params/catalogue/fx.h"

#include <algorithm>

namespace core
{
namespace
{
model::EffectSettings settingsOf(const model::EffectSlot& s)
{
    model::EffectSettings e;
    e.type = s.type;
    e.type1 = s.type1;
    e.type2 = s.type2;
    e.params = s.params;
    e.sync = s.sync;
    e.lookAhead = s.lookAhead;
    return e;
}

const fx::EffectParamSet* paramsOf(model::EffectType t)
{
    (void)builtinParams(); // the catalogues (fx_<name>.cpp) register their defs when the registry is built
    return fx::effectParams(fx::effectIdOf(t));
}
} // namespace

model::EffectSlot& fxSlotAt(model::Project& p, const model::ParamPath& at)
{
    auto& scene = p.scenes[at.scene];
    switch (at.scope)
    {
    case model::Scope::SendFx:
        return scene.sendFx;
    case model::Scope::MasterFx:
        return scene.masterFx;
    default:
        return scene.tracks[at.track].insertFx[at.slot & 1];
    }
}

const model::EffectSlot& fxSlotAt(const model::Project& p, const model::ParamPath& at)
{
    return fxSlotAt(const_cast<model::Project&>(p), at);
}

std::string fxSlotPath(const model::ParamPath& at)
{
    const std::string scene = "scene[" + std::to_string(at.scene) + "]";
    switch (at.scope)
    {
    case model::Scope::SendFx:
        return scene + ".sendFx";
    case model::Scope::MasterFx:
        return scene + ".masterFx";
    default:
        break;
    }
    return scene + ".track[" + std::to_string(at.track) + "].insertFx[" + std::to_string(at.slot & 1) + "]";
}

std::uint8_t fxSlotBit(const model::ParamPath& at) noexcept
{
    switch (at.scope)
    {
    case model::Scope::SendFx:
        return fx::kSlotSend;
    case model::Scope::MasterFx:
        return fx::kSlotMaster;
    default:
        break;
    }
    return fx::kSlotInsert;
}

bool fxTypeAllowed(const model::ParamPath& at, model::EffectType t) noexcept
{
    if (t == model::EffectType::Off)
        return true;
    return fxSlotBit(at) == fx::kSlotInsert ? model::effectAllowedOnInsert(t) : model::effectAllowedOnBus(t);
}

void applyFxDefaults(model::EffectSlot& s, model::EffectType type, std::uint8_t slotBit)
{
    s.params.fill(0.0);
    s.sync = s.lookAhead = false;
    s.type1 = s.type2 = 0;
    if (const auto* set = paramsOf(type))
    {
        for (const auto& d : set->params)
            if (d.k >= 0 && d.k < 6)
                s.params[static_cast<std::size_t>(d.k)] = static_cast<double>(fx::defaultFor(d, slotBit));
        for (const auto& o : set->options)
        {
            const float v =
                slotBit == fx::kSlotSend ? o.defSend : (slotBit == fx::kSlotMaster ? o.defMaster : o.defInsert);
            if (o.key == "type1")
                s.type1 = static_cast<int>(v);
            else if (o.key == "type2")
                s.type2 = static_cast<int>(v);
        }
    }
}

std::unique_ptr<Command> RestoreFxSlot::inverse(const model::Project& before) const
{
    return std::make_unique<RestoreFxSlot>(at_, fxSlotAt(before, at_), label_);
}

std::size_t RestoreFxSlot::sizeBytes() const
{
    return sizeof(*this) + slot_.typeMemory.capacity() * sizeof(model::EffectSettings);
}

void SelectFxType::apply(model::Project& p)
{
    auto& s = fxSlotAt(p, at_);
    if (! fxTypeAllowed(at_, type_) || (type_ == model::EffectType::Plugin && plugin_.pluginId.empty()))
        return;
    s.plugin = type_ == model::EffectType::Plugin ? plugin_ : model::PluginTrack{};
    if (s.type == type_)
        return;
    auto& mem = s.typeMemory;
    if (s.type != model::EffectType::Off) // FX-007: remember the settings this type leaves with
    {
        auto it = std::find_if(mem.begin(), mem.end(), [&](const auto& e) { return e.type == s.type; });
        if (it != mem.end())
            *it = settingsOf(s);
        else
            mem.push_back(settingsOf(s));
    }
    auto it = std::find_if(mem.begin(), mem.end(), [&](const auto& e) { return e.type == type_; });
    if (type_ == model::EffectType::Off)
    {
        s.params.fill(0.0); // FX-091
        s.sync = s.lookAhead = false;
        s.type1 = s.type2 = 0;
    }
    else if (it != mem.end()) // FX-006: the type's last values in this slot position
    {
        s.type1 = it->type1;
        s.type2 = it->type2;
        s.params = it->params;
        s.sync = it->sync;
        s.lookAhead = it->lookAhead;
        mem.erase(it);
    }
    else
    {
        applyFxDefaults(s, type_, fxSlotBit(at_)); // FX-092
        if (type_ == model::EffectType::Ducker && fxSlotBit(at_) == fx::kSlotInsert)
            s.type1 = at_.track; // FX-1262: a new Ducker's Source is its own track
    }
    s.type = type_;
}

std::unique_ptr<Command> SelectFxType::inverse(const model::Project& before) const
{
    return std::make_unique<RestoreFxSlot>(at_, fxSlotAt(before, at_), "FX TYPE");
}

void ToggleFxSync::apply(model::Project& p)
{
    auto& s = fxSlotAt(p, at_);
    const auto* set = paramsOf(s.type);
    if (set == nullptr)
        return;
    bool any = false;
    const bool toSync = ! s.sync;
    for (const auto& d : set->params)
    {
        if (! fx::hasSync(d.family) || d.k < 0 || d.k >= 6)
            continue;
        auto& v = s.params[static_cast<std::size_t>(d.k)];
        v = static_cast<double>(fx::convertSync(static_cast<float>(v), d.family, toSync, bpm_)); // FX-073
        any = true;
    }
    if (any)
        s.sync = toSync;
}

std::unique_ptr<Command> ToggleFxSync::inverse(const model::Project& before) const
{
    return std::make_unique<RestoreFxSlot>(at_, fxSlotAt(before, at_), "SYNC");
}

std::unique_ptr<Command> makeSetFxBypass(const model::ParamPath& at, bool on)
{
    // Every slot kind: an insert (FX-045) or the scene's Send / Master slot (FX-046).
    constexpr auto field = [](model::Project& p, const model::ParamPath& a) -> bool& { return fxSlotAt(p, a).bypass; };
    return makeSetParam<bool>(fxSlotPath(at) + ".bypass", "FX BYPASS", field, at, on, SetParam<bool>::Coalesce::No);
}

std::unique_ptr<Command> makeSetFxMode(std::uint8_t scene, model::FxMode mode)
{
    constexpr auto field = [](model::Project& p, const model::ParamPath& a) -> model::FxMode&
    { return p.scenes[a.scene].routing.fxMode; };
    model::ParamPath at{};
    at.scope = model::Scope::Scene;
    at.scene = scene;
    return makeSetParam<model::FxMode>("scene[" + std::to_string(scene) + "].routing.fxMode", "FX MODE", field, at,
                                       mode, SetParam<model::FxMode>::Coalesce::No);
}
} // namespace core
