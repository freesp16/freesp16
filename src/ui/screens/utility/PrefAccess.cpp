#include "ui/screens/utility/PrefAccess.h"

#include "ui/screens/utility/UtilityEnv.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace ui::utility
{
namespace st = services::settings;

void PreferenceAccess::set(const core::ParamSpec& s, std::uint16_t scope, float v)
{
    inner_.set(s, scope, v);
    if (st::persists(s))
        prefs_.set(st::keyOf(s, scope), s.clamp(v));
}

int applyPreferences(const st::Preferences& prefs, ParamAccess& values, const core::ParamRegistry& reg)
{
    int n = 0;
    for (const auto& [key, value] : prefs.values())
    {
        std::string_view base = key;
        std::uint16_t scope = 0;
        if (const auto hash = base.find('#'); hash != std::string_view::npos)
        {
            scope = static_cast<std::uint16_t>(std::strtoul(key.c_str() + hash + 1, nullptr, 10));
            base = base.substr(0, hash);
        }
        const core::ParamSpec* spec = nullptr;
        for (const auto& p : reg.all())
            if (p.storageKey() == base && st::persists(p))
            {
                spec = &p;
                break;
            }
        if (spec == nullptr)
            continue; // a PrefSpec value (read through Preferences) or a key of a newer build
        values.set(*spec, scope, spec->clamp(value));
        ++n;
    }
    return n;
}

UtilityEnv& utilityEnv() noexcept
{
    static UtilityEnv env;
    return env;
}

std::string formatBytes(double bytes)
{
    char buf[32];
    if (bytes < 0)
        return "\xE2\x80\x94";
    if (bytes >= 1e9)
        std::snprintf(buf, sizeof buf, "%.1f GB", bytes / 1e9);
    else
        std::snprintf(buf, sizeof buf, "%.0f MB", bytes / 1e6);
    return buf;
}
} // namespace ui::utility
