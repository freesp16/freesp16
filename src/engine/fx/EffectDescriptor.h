// The engine half of an effect's EffectSpec (07-ui-design/05-extensibility.md UID-215/216; effect-shell.md FX-020…057):
// data only, no DSP and no engine state, so ui/ may include it to generate the SELECT FX grid and the generic effect
// panel (UID-216/217) from the registry.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace engine::fx
{
class EffectBase;

enum class SlotKind : std::uint8_t { Insert = 1, Send = 2, Master = 4 };
constexpr std::uint8_t operator| (SlotKind a, SlotKind b) noexcept
{
    return static_cast<std::uint8_t>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
constexpr bool allows(std::uint8_t slots, SlotKind kind) noexcept { return (slots & static_cast<std::uint8_t>(kind)) != 0; }

// Shell policy per effect (FX-027 gate mode, FX-028 10 Hz wet high-pass, FX-030 mix law, skip/clamp) — data, not code:
struct ShellPolicy { std::uint8_t gateMode, mixLaw; bool wetHighPass, skipWhenSilent, clampOutput; };
struct EffectDescriptor {                              // engine half of the spec's EffectSpec (UID-215)
    std::string_view id, name; std::uint8_t slots;     // SlotKind mask
    std::array<std::string_view, 6> params;            // ParamSpec ids in page order, "" = empty knob
    std::span<const std::string_view> options;         // enum/bool ParamSpec ids for the option row
    std::string_view visual;                           // meters | duckEnvelope | eqCurve | ...
    ShellPolicy shell {};                              // how FxShell wraps this effect (FX-027…031)
    std::unique_ptr<EffectBase> (*create)();
    std::string_view featureFlag {};
};
} // namespace engine::fx
