// The reference slot orders and the checks of the effect registry (FX-040, UID-207/831). The
// EffectRegistry (EffectRegistry.h, frozen) stores descriptors in registration order; this file adds what the slots,
// SELECT FX and the contract test need, without touching it:
//
//   listFor(registry, kind, enabledFlags)  the types a slot kind offers, in slot order: the reference order below
//       (ids not registered yet are skipped), then every other registered id allowed in the slot,
//       in id order (so static-initialisation order never decides the grid). A descriptor with a feature flag is
//       listed only when that flag is in enabledFlags (FX-001, FX-011: unavailable entries are not listed, UID-831).
//       The index in this list is the slot's type index (FxProcessor::requestType, the "<slot>.type" value − 1).
//   validate(registry, paramIds)  one readable line per problem (bad slot mask, > 6 / unknown parameters or options,
//       no factory, bad id), "" lines never; paramIds = every ParamSpec id (core::ParamRegistry order).
//
// Data only and no engine state, like EffectRegistry.h: the UI may use it (it needs a tools/check-deps.py entry next
// to EffectRegistry.h). Message thread only (allocates).
//
// Parameter ids. A descriptor names its knobs and options slot-independently as "fx.<effect>.<key>"; the
// ParamSpecs of one slot are "<slot>.<effect>.<key>" with slot ∈ {fx1, fx2, send, master}
// (core::fx::registerEffect). slotParamId() converts.
#pragma once

#include "engine/fx/EffectRegistry.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::fx
{
constexpr std::uint8_t kAllSlotKinds = static_cast<std::uint8_t>(SlotKind::Insert) |
                                       static_cast<std::uint8_t>(SlotKind::Send) |
                                       static_cast<std::uint8_t>(SlotKind::Master);
using Text = std::string;                               // rt-ok: message-thread registry helpers (ids, readable errors)
constexpr std::string_view kTestEffectFlag = "fx.test"; // None, GainTest and test-only effects: never listed in the UI

// FX-040 reference orders (ids; each effect registers its own id).
std::span<const std::string_view> referenceOrder(SlotKind) noexcept;

std::vector<const EffectDescriptor*> listFor(const EffectRegistry&, SlotKind,
                                             std::span<const std::string_view> enabledFlags = {});
int typeIndexOf(std::span<const EffectDescriptor* const> list, std::string_view id) noexcept; // −1 = not listed

// Integration: the engine's slot bank instantiates every registered type, so it builds its lists with every
// feature flag the registry carries; whoever maps an effect id to the engine's type index (the model → engine
// translator, ui/screens/effects/ModelFxSlots) uses the same lists: engineTypes(registry, kind).
std::vector<std::string_view> allFeatureFlags(const EffectRegistry&);
std::vector<const EffectDescriptor*> engineTypes(const EffectRegistry&, SlotKind);

// "fx.<effect>.<key>" → "<slotPrefix>.<effect>.<key>" (slotPrefix "fx1", "fx2", "send", "master"); "" when the id
// does not start with "fx.".
Text slotParamId(std::string_view slotPrefix, std::string_view descriptorParamId);
std::span<const std::string_view> slotPrefixes(SlotKind) noexcept; // Insert: fx1, fx2; Send: send; Master: master

std::vector<Text> validate(const EffectRegistry&, std::span<const std::string_view> paramIds);
std::vector<Text> validateDescriptor(const EffectDescriptor&, std::span<const std::string_view> paramIds);
} // namespace engine::fx
