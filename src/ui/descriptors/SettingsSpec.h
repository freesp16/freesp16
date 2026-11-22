// Settings descriptors (07-ui-design/05-extensibility.md §5, UID-220): UTILITY is a SettingsSpec of sections →
// rows; a row is a `value` (inline enum/number bound to a ParamSpec, usually a Global preference), an `action` (push →
// popup or screen) or `info`. New settings append to a section or add a section; existing rows never move — rows are
// therefore registered per section with an explicit order key (APP_REGISTER_SETTINGS_ROW), never by editing a list.
#pragma once

#include "core/util/Registrar.h"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

namespace ui
{
struct SettingsRowSpec
{
    enum class Kind : std::uint8_t { Value, Action, Info } kind;
    std::string_view label;
    std::string_view binds {};       // Value: ParamSpec id
    std::string_view opens {};       // Action: ScreenSpec / PopupSpec id
    std::string_view infoSource {};  // Info: data source key (version, storage, …)
    std::string_view featureFlag {};
};

struct SettingsSectionSpec
{
    std::string_view settingsId; // the SettingsSpec it belongs to ("utility")
    std::string_view id, title;
    int order = 0;               // section position (reference sections keep their spec order; extensions go last)
};

struct SettingsRowEntry
{
    std::string_view settingsId, sectionId;
    int order = 0;               // row position within its section
    SettingsRowSpec row;
};

struct SettingsSpec
{
    std::string_view id, title;  // "utility", "UTILITY"
};

class SettingsRegistry
{
public:
    static SettingsRegistry& builtin() noexcept
    {
        static SettingsRegistry registry;
        return registry;
    }
    void add(const SettingsSpec& s) { specs_.push_back(s); }
    void add(const SettingsSectionSpec& s) { sections_.push_back(s); }
    void add(const SettingsRowEntry& r) { rows_.push_back(r); }

    std::vector<SettingsSectionSpec> sections(std::string_view settingsId) const
    {
        std::vector<SettingsSectionSpec> out;
        for (const auto& s : sections_)
            if (s.settingsId == settingsId)
                out.push_back(s);
        std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.order < b.order; });
        return out;
    }
    std::vector<SettingsRowSpec> rows(std::string_view settingsId, std::string_view sectionId) const
    {
        std::vector<SettingsRowEntry> entries;
        for (const auto& r : rows_)
            if (r.settingsId == settingsId && r.sectionId == sectionId)
                entries.push_back(r);
        std::stable_sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.order < b.order; });
        std::vector<SettingsRowSpec> out;
        for (const auto& e : entries)
            out.push_back(e.row);
        return out;
    }
    const std::vector<SettingsSpec>& all() const noexcept { return specs_; }

private:
    std::vector<SettingsSpec> specs_;
    std::vector<SettingsSectionSpec> sections_;
    std::vector<SettingsRowEntry> rows_;
};
} // namespace ui

// APP_REGISTER_SETTINGS((ui::SettingsSpec{...})), (ui::SettingsSectionSpec{...}) or (ui::SettingsRowEntry{...})
#define APP_REGISTER_SETTINGS(...) APP_STATIC_REGISTRAR(::ui::SettingsRegistry::builtin().add(__VA_ARGS__))
