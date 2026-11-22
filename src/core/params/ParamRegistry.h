// Registry of every ParamSpec (UID-200). Registration happens at start-up on the message thread, before any engine
// or UI object is created; afterwards the registry is immutable and may be read from any thread.
#pragma once

#include "core/params/ParamSpec.h"
#include "core/util/Registrar.h"

#include <optional>
#include <vector>

namespace core
{
class ParamRegistry
{
public:
    // Registers a spec. Fails (returns an invalid id) on a duplicate id, an empty label, or an enum without labels.
    // Errors are collected in errors() so a test can list every bad descriptor at once.
    ParamId add(const ParamSpec& spec);

    const ParamSpec* find(std::string_view id) const noexcept;
    std::optional<ParamId> idOf(std::string_view id) const noexcept;
    const ParamSpec& operator[] (ParamId id) const noexcept { return specs_[id.value]; }
    std::size_t size() const noexcept { return specs_.size(); }
    std::span<const ParamSpec> all() const noexcept { return specs_; }
    const std::vector<std::string_view>& errors() const noexcept { return errors_; }

    // Generated views (UID-201): every consumer derives its list from the registry, never from a hand-written table.
    std::vector<ParamId> lockable() const;
    std::vector<ParamId> lfoTargets() const;
    std::vector<ParamId> stripTargets() const;
    std::optional<ParamId> byMidiCc(int cc) const noexcept;
    std::vector<ParamId> byScope(ParamScope) const; // e.g. Global = every preference (SET), in registration order

private:
    std::vector<ParamSpec> specs_;
    std::vector<std::string_view> errors_;
};

// The built-in catalogue: one file per area, core/params/catalogue/<area>.cpp, so two areas never
// share a file. Each file registers its specs with APP_REGISTER_PARAMS(area, order, fn); the
// catalogue functions run sorted by (order, area), so ParamIds are dense and stable within one build whatever the
// link order. Order bands (tens, so an area can be inserted between two others):
//   100 track · 200 playback · 300 envelope · 400 sequencer · 500 mixer · 600 lfo · 700 fx (700 + effect)
//   800 performance/ribbon · 900 sampling/render · 1000 midi · 1100 project/scene · 1200 settings (Global)
using ParamCatalogueFn = void (*)(ParamRegistry&);
struct ParamCatalogue
{
    std::string_view area; // file name without extension: "track", "playback", "fx_tremolo", ...
    int order = 0;
    ParamCatalogueFn add = nullptr;
};
void registerParamCatalogue(const ParamCatalogue&); // static registration only (APP_REGISTER_PARAMS)
std::vector<ParamCatalogue> paramCatalogues();       // sorted by (order, area)

void registerBuiltinParams(ParamRegistry&); // runs every registered catalogue, sorted
ParamRegistry& builtinParams();             // lazily built once, then immutable
} // namespace core

#define APP_REGISTER_PARAMS(area, order, fn) APP_STATIC_REGISTRAR(::core::registerParamCatalogue({area, order, fn}))
