// Column state rules (UI-003, ParamSpec `dependsOn` of 05-extensibility.md §1): a parameter's column is drawn greyed
// and ignores its encoder while a rule says so (e.g. PITCH while TIME STRETCH = RESAMPLE). The owning package registers
// the rule next to its parameters; the shell evaluates it for every bound column each frame (no allocation).
#pragma once

#include "core/util/Registrar.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace core
{
class ParamRegistry;
}

namespace ui
{
class ParamAccess;

using ColumnDisabledFn = bool (*)(const ParamAccess&, const core::ParamRegistry&, std::uint16_t scope);

class ColumnRuleRegistry
{
public:
    struct Entry
    {
        std::string_view param;
        ColumnDisabledFn disabled;
    };
    static ColumnRuleRegistry& builtin() noexcept
    {
        static ColumnRuleRegistry registry;
        return registry;
    }
    void add(std::string_view param, ColumnDisabledFn fn) { entries_.push_back({param, fn}); }
    ColumnDisabledFn find(std::string_view param) const noexcept
    {
        for (const auto& e : entries_)
            if (e.param == param)
                return e.disabled;
        return nullptr;
    }

private:
    std::vector<Entry> entries_;
};
} // namespace ui

#define APP_REGISTER_COLUMN_RULE(param, fn) APP_STATIC_REGISTRAR(::ui::ColumnRuleRegistry::builtin().add(param, fn))
