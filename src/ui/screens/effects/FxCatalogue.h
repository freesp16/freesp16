// What the effect UI derives from the registries (UID-215…217, UID-831): the slot's effect list in SELECT FX order,
// and each effect's per-slot ParamSpecs. No per-effect code: a newly registered effect is listed and resolved here
// without any change to this package.
//
// Slot order (UID-831, [OBS] 1.60): the reference ids first in the reference order (ids not registered are skipped),
// then the P3 extensions in their spec order, then any other registered id in id order (so static-initialisation order
// never decides the grid). An effect with a feature flag is listed only while that flag is on (OQ-08, UID-833); NONE
// is always the first cell (synthesised unless the registry has an unflagged "none" descriptor: FXCORE's own "none" is
// a test effect behind fx.test, never the UI's NONE).
// Membership comes from engine::fx::listFor (engine/fx/EffectRegistryOrder.h: slot mask and flags); the order above
// is SELECT FX's. The engine's type index is a different list (FX-040 processor order,
// engine::fx::engineTypes): never use a grid position as a type index.
//
// ParamSpec ids (screens/effects.md "The 12 effects"): a descriptor names `fx.<effect>.<key>`; slot <s> instantiates
// it as `<s>.<effect>.<key>` (fx1, fx2, send, master). A descriptor may also name the bare key or `<effect>.<key>`.
// When no per-slot spec is registered the descriptor's id itself is used (an effect registered once for every slot).
// SYNC form (our choice): with SYNC on, a column whose ParamSpec has a sibling `<id>.div` (label DIV, or the Ducker's
// sixteenths with its own caption) is bound to that sibling instead (UID-830, FX-071). core::fx::registerEffect
// registers it for every SYNC family (the same knob k as a step fraction of the sync grid, FX-068 sync text).
#pragma once

#include "core/params/ParamRegistry.h"
#include "engine/fx/EffectRegistry.h"
#include "ui/screens/effects/FxSlots.h"
#include "ui/shell/FeatureFlags.h"

#include <span>
#include <string_view>
#include <vector>

namespace ui::fx
{
// The flag that enables every P3 extension effect at once ("Sixteen extensions", FX-001 / UID-833).
inline constexpr std::string_view kExtensionsFlag = "fx.extensions";

std::span<const std::string_view> referenceOrder(engine::fx::SlotKind) noexcept;
std::span<const std::string_view> extensionOrder() noexcept;

// The NONE entry used when the registry has none (id "none", name "NONE", every slot, no parameters).
const engine::fx::EffectDescriptor& noneDescriptor() noexcept;

// True when `d` may be listed with `flags` (no flag, its own flag, or the extensions flag for a flagged effect).
bool listed(const engine::fx::EffectDescriptor& d, const FeatureFlags& flags) noexcept;
// True for P3 extensions (a feature-flagged effect): the grid shows the EXT badge.
inline bool isExtension(const engine::fx::EffectDescriptor& d) noexcept
{
    return ! d.featureFlag.empty();
}

// Fills `out` (cleared) with the effects slot kind `k` may run, in SELECT FX order. Allocates (popup open only).
void listFor(const engine::fx::EffectRegistry&, engine::fx::SlotKind k, const FeatureFlags&,
             std::vector<const engine::fx::EffectDescriptor*>& out);

// The descriptor of `id` (NONE for an empty id or "none" without a registered descriptor), or null.
const engine::fx::EffectDescriptor* findEffect(const engine::fx::EffectRegistry&, std::string_view id) noexcept;

// The per-slot ParamSpec of descriptor parameter/option id `param`, or null. No allocation.
const core::ParamSpec* resolveParam(const core::ParamRegistry&, Slot, const engine::fx::EffectDescriptor&,
                                    std::string_view param) noexcept;
// The SYNC form `<spec.id>.div` of a resolved spec, or null. No allocation.
const core::ParamSpec* syncForm(const core::ParamRegistry&, const core::ParamSpec&) noexcept;
// The option ParamSpec of the descriptor whose key is `sync` (SYNC toggle, FX-071), or null.
const core::ParamSpec* syncOption(const core::ParamRegistry&, Slot, const engine::fx::EffectDescriptor&) noexcept;

// The engine::ParamChange scope of `spec` as the shell computes it (track params: selected track of the scene).
std::uint16_t scopeOf(const core::ParamSpec&, int scene, int track) noexcept;
} // namespace ui::fx
