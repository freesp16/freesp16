// ParamId → FX slot target (parameters, locks, type memory): a flat POD table indexed by
// ParamId.value, built on the message thread from the registry's id list, so the engine never sees the ParamRegistry
// and the audio thread resolves a ParamChange with one array read.
//
// Ids (core::fx::registerEffect and core/params/catalogue/fx.cpp):
//   <slot>.<effect>.<key>   Knob k of that effect's instance in that slot (slot ∈ fx1, fx2, send, master), or
//                           Option i when "fx.<effect>.<key>" is the descriptor's options[i]
//   <slot>.type             SlotType, value = type index + 1 (0 = Off)
//   <slot>.bypass           SlotBypass (every slot: FX-044 inserts, FX-046 Send/Master), value 0/1
//   fx.mode                 Mode, value FxMode (per scene)
//   fx.quirkProfile         Quirk, value 0 = Sixteen, 1 = Reference (project)
//   fx.meteredSlot          Meter, value = the slot the effect panel shows (0…33), −1 = none (UI session state)
//   <slot>.<effect>.<key>.div   the SYNC form of a time/rate knob (UID-830): the same Knob k (the value is the step
//                           fraction of the sync grid; core::fx::registerEffect registers it for SYNC families)
// k (the internal index, FX-060) is `internalIndex[ParamId]` when given (core::fx::internalIndexTable), else the
// knob's page position in the descriptor (TREMOLO: identical).
#pragma once

#include "core/params/ParamId.h"
#include "engine/fx/EffectDescriptor.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace engine::fx
{
struct FxRoute
{
    enum class Kind : std::uint8_t
    {
        None,
        Knob,
        Option,
        SlotType,
        SlotBypass,
        Mode,
        Quirk,
        Meter
    };
    Kind kind = Kind::None;
    SlotKind slotKind = SlotKind::Insert;
    std::uint8_t unit = 0;  // Insert: 0 = FX1, 1 = FX2
    std::int8_t type = -1;  // Knob/Option: the type index in the slot kind's list
    std::int8_t index = -1; // Knob: k; Option: which
};

class FxSlotRoutes
{
public:
    // The type lists of the three slot kinds (listFor order); paramIds in registry order (index = ParamId.value).
    void build(std::span<const EffectDescriptor* const> insertTypes, std::span<const EffectDescriptor* const> sendTypes,
               std::span<const EffectDescriptor* const> masterTypes, std::span<const std::string_view> paramIds,
               std::span<const std::int8_t> internalIndex = {});
    const FxRoute& find(core::ParamId id) const noexcept
    {
        return id.value < routes_.size() ? routes_[id.value] : none_;
    }
    std::size_t size() const noexcept { return routes_.size(); }
    std::size_t routed() const noexcept; // entries that are not None

    static FxRoute parse(std::string_view id, std::span<const EffectDescriptor* const> insertTypes,
                         std::span<const EffectDescriptor* const> sendTypes,
                         std::span<const EffectDescriptor* const> masterTypes, std::int8_t k = -1);

private:
    std::vector<FxRoute> routes_;
    FxRoute none_{};
};
} // namespace engine::fx
