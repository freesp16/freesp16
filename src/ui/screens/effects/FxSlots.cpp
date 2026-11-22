#include "ui/screens/effects/FxSlots.h"

#include "ui/widgets/EffectPanel.h"

namespace ui::fx
{
LocalFxSlots& LocalFxSlots::shared() noexcept
{
    static LocalFxSlots slots;
    return slots;
}

LocalFxSlots::Entry& LocalFxSlots::at(Slot s, int scene, int track) noexcept
{
    const auto sc = static_cast<std::size_t>((scene - 1) & 15);
    if (isInsert(s))
        return inserts_[s == Slot::Fx1 ? 0 : 1][sc][static_cast<std::size_t>((track - 1) & 15)];
    return bus_[s == Slot::Send ? 0 : 1][sc];
}

const LocalFxSlots::Entry& LocalFxSlots::at(Slot s, int scene, int track) const noexcept
{
    return const_cast<LocalFxSlots*>(this)->at(s, scene, track);
}

std::string_view LocalFxSlots::effect(Slot s, int scene, int track) const
{
    return at(s, scene, track).effect;
}

void LocalFxSlots::selectEffect(Slot s, int scene, int track, std::string_view effectId)
{
    at(s, scene, track).effect = isNone(effectId) ? kNone : effectId;
    ++changes_;
}

bool LocalFxSlots::bypass(Slot s, int scene, int track) const
{
    return at(s, scene, track).bypass;
}

void LocalFxSlots::setBypass(Slot s, int scene, int track, bool on)
{
    at(s, scene, track).bypass = on;
    ++changes_;
}

FxMode LocalFxSlots::mode(int scene) const
{
    return modes_[static_cast<std::size_t>((scene - 1) & 15)];
}

void LocalFxSlots::setMode(int scene, FxMode m)
{
    modes_[static_cast<std::size_t>((scene - 1) & 15)] = m;
    ++changes_;
}

void LocalFxSlots::reset() noexcept
{
    inserts_ = {};
    bus_ = {};
    modes_ = {};
    ++changes_;
}

const engine::fx::EffectRegistry& FxEnvironment::effectRegistry() const noexcept
{
    return effects != nullptr ? *effects : engine::fx::EffectRegistry::builtin();
}

const EffectUiRegistry& FxEnvironment::uiRegistry() const noexcept
{
    return effectUi != nullptr ? *effectUi : EffectUiRegistry::builtin();
}

const PanelHintsRegistry& FxEnvironment::hintsRegistry() const noexcept
{
    return hints != nullptr ? *hints : PanelHintsRegistry::builtin();
}

FxSlotAccess& FxEnvironment::slotAccess() const noexcept
{
    return slots != nullptr ? *slots : LocalFxSlots::shared();
}

FxEnvironment& environment() noexcept
{
    static FxEnvironment env;
    return env;
}
Slot& busPage() noexcept
{
    static Slot page = Slot::Send;
    return page;
}
} // namespace ui::fx
