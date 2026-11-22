// List popups (07-ui-design/05-extensibility.md §5, UID-221): every list popup (TRACK TYPE, SHAPE, FILTER TYPE,
// DESTINATION …) is a ListSpec with a data source, grouping and an optional icon column. Sources register by name
// (APP_REGISTER_LIST_SOURCE) — e.g. "params.lfoTargets" generated from the ParamRegistry (UID-201).
#pragma once

#include "core/util/Registrar.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace ui
{
struct ListItem
{
    std::string_view label;
    std::string_view group {};   // grouping header ("PLAYBACK"); empty = ungrouped
    std::string_view icon {};    // icon id for the icon column
    int value = 0;               // what selecting the row sets (enum index, ParamId, …)
    bool enabled = true;
};

// Fills `out` (cleared by the caller) for the given argument (e.g. the bound path). Message thread.
using ListSourceFn = void (*)(std::string_view argument, std::vector<ListItem>& out);

struct ListSpec
{
    std::string_view id, title;
    std::string_view source;     // ListSourceRegistry key
    std::string_view argument {};
    bool grouped = false;
    bool iconColumn = false;
    std::string_view binds {};   // ParamSpec id the selection writes, if any
};

class ListSourceRegistry
{
public:
    struct Entry
    {
        std::string_view name;
        ListSourceFn fill;
    };
    static ListSourceRegistry& builtin() noexcept
    {
        static ListSourceRegistry registry;
        return registry;
    }
    void add(std::string_view name, ListSourceFn fill) { entries_.push_back({name, fill}); }
    ListSourceFn find(std::string_view name) const noexcept
    {
        for (const auto& e : entries_)
            if (e.name == name)
                return e.fill;
        return nullptr;
    }

private:
    std::vector<Entry> entries_;
};
} // namespace ui

#define APP_REGISTER_LIST_SOURCE(name, fill) APP_STATIC_REGISTRAR(::ui::ListSourceRegistry::builtin().add(name, fill))
