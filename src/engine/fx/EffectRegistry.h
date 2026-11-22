// The effect registry (UID-215/216): every effect registers its EffectDescriptor with APP_REGISTER_EFFECT in its own
// file (engine/fx/effects/<Name>.cpp); the SELECT FX grid, panel, p-locks, LFO targets and MIDI CCs follow from it.
// Data only (ui/ may include it). Registration happens before main() on the main
// thread; the registry is read-only afterwards and never touched by the audio thread (the FX bank builds every
// effect instance at start-up, threading.md "Effect types do not allocate on change").
//
// This file provides registration and lookup; EffectRegistryOrder.h adds the reference slot orders (listFor).
#pragma once

#include "core/util/Registrar.h"
#include "engine/fx/EffectDescriptor.h"

#include <string_view>
#include <vector>

namespace engine::fx
{
class EffectRegistry
{
public:
    static EffectRegistry& builtin() noexcept
    {
        static EffectRegistry registry;
        return registry;
    }

    // False on an empty or duplicate id (the descriptor is ignored; the error is listed by errors()).
    bool add(const EffectDescriptor& d)
    {
        if (d.id.empty() || find(d.id) != nullptr)
        {
            errors_.push_back(d.id); // rt-ok: start-up registration, main thread
            return false;
        }
        effects_.push_back(d); // rt-ok: start-up registration, main thread
        return true;
    }
    const EffectDescriptor* find(std::string_view id) const noexcept
    {
        for (const auto& e : effects_)
            if (e.id == id)
                return &e;
        return nullptr;
    }
    const std::vector<EffectDescriptor>& all() const noexcept { return effects_; } // registration order
    const std::vector<std::string_view>& errors() const noexcept { return errors_; }

private:
    std::vector<EffectDescriptor> effects_;
    std::vector<std::string_view> errors_;
};
} // namespace engine::fx

// APP_REGISTER_EFFECT(descriptor): static registrar into EffectRegistry::builtin(), e.g.
//   APP_REGISTER_EFFECT((engine::fx::EffectDescriptor{"tremolo", "TREMOLO", ...}))
#define APP_REGISTER_EFFECT(...) APP_STATIC_REGISTRAR(::engine::fx::EffectRegistry::builtin().add(__VA_ARGS__))
