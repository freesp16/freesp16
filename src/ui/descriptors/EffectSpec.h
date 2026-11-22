// The UI half of an effect's EffectSpec (07-ui-design/05-extensibility.md §4, UID-215…217). The engine half — id, name,
// slots, the six ParamSpec ids in page order, option ids, visual — is engine::fx::EffectDescriptor
// (engine/fx/EffectDescriptor.h, data only, which ui/ may include); this adds what only the panel needs: the icon, how
// each option is shown, and the visual widget. The panel layout is generic (UID-217): no effect has a bespoke screen.
// Registered with APP_REGISTER_EFFECT_UI next to the effect's ParamSpecs (the generic panel is generated from it).
#pragma once

#include "core/util/Registrar.h"
#include "engine/fx/EffectDescriptor.h"
#include "engine/fx/EffectRegistry.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace ui
{
struct EffectOptionUi
{
    enum class Control : std::uint8_t { Selector, Segmented, Toggle } control; // option row control (UID-215)
    std::string_view param;                                                    // ParamSpec id (enum/bool)
};

struct EffectUiSpec
{
    std::string_view effectId;               // engine::fx::EffectDescriptor::id
    std::string_view icon;                   // resources/icons/fx/<icon>.svg (24/48 lpx family)
    std::span<const EffectOptionUi> options; // in option-row order; empty = selectors for every option
    std::string_view visualWidget {};        // widget key, default "fxvisual.<descriptor.visual>"
};

class EffectUiRegistry
{
public:
    static EffectUiRegistry& builtin() noexcept
    {
        static EffectUiRegistry registry;
        return registry;
    }
    void add(const EffectUiSpec& s) { specs_.push_back(s); }
    const EffectUiSpec* find(std::string_view effectId) const noexcept
    {
        for (const auto& s : specs_)
            if (s.effectId == effectId)
                return &s;
        return nullptr;
    }
    const std::vector<EffectUiSpec>& all() const noexcept { return specs_; }

private:
    std::vector<EffectUiSpec> specs_;
};
} // namespace ui

#define APP_REGISTER_EFFECT_UI(...) APP_STATIC_REGISTRAR(::ui::EffectUiRegistry::builtin().add(__VA_ARGS__))
