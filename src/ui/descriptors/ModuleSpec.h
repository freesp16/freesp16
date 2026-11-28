// TRACK MENU signal-chain modules (07-ui-design/05-extensibility.md §3; UID-210 reference order per track type,
// UID-211 six fixed slots + a second page, never moving reference modules). A module is registered with
// APP_REGISTER_MODULE in the file of the feature it opens; the chain for a track type is a ModuleChainSpec.
#pragma once

#include "core/model/enums/Enums.h"
#include "core/util/Registrar.h"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace ui
{
struct ModuleSpec
{
    std::string_view id;           // "fx1"
    std::string_view title;        // "FX1 : {fx1.type}" — {path} is substituted with the bound value's label
    std::string_view thumbnail;    // widget key: EffectIcon(fx1.type), WaveformThumb, EnvelopeThumb, LfoThumb, …
    std::string_view strip;        // strip binding on first tap: "fx1.panel.strip"
    std::string_view opens;        // ScreenSpec id opened on the second tap: "track.fx1"
    std::uint8_t enabledFor = 0x1F; // bit per core::model::TrackMode (Sample, Midi, Thru, Instrument, Plugin)
    bool connector = true;         // draws the chain line to the next module
    std::string_view featureFlag {};
};

// The chain row of one track type: module ids per slot ("" = empty slot), page 0 = the reference row (UID-210).
struct ModuleChainSpec
{
    core::model::TrackMode mode;
    std::uint8_t page = 0;
    std::array<std::string_view, 6> slots;
};

class ModuleRegistry
{
public:
    static ModuleRegistry& builtin() noexcept
    {
        static ModuleRegistry registry;
        return registry;
    }
    void add(const ModuleSpec& m) { modules_.push_back(m); }
    void add(const ModuleChainSpec& c) { chains_.push_back(c); }
    const ModuleSpec* find(std::string_view id) const noexcept
    {
        for (const auto& m : modules_)
            if (m.id == id)
                return &m;
        return nullptr;
    }
    const std::vector<ModuleSpec>& modules() const noexcept { return modules_; }
    const std::vector<ModuleChainSpec>& chains() const noexcept { return chains_; }

private:
    std::vector<ModuleSpec> modules_;
    std::vector<ModuleChainSpec> chains_;
};
} // namespace ui

// APP_REGISTER_MODULE((ui::ModuleSpec{...})) or APP_REGISTER_MODULE((ui::ModuleChainSpec{...}))
#define APP_REGISTER_MODULE(...) APP_STATIC_REGISTRAR(::ui::ModuleRegistry::builtin().add(__VA_ARGS__))
