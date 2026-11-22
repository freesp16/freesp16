#include "core/commands/arrangement/ArrangementPlanBuilder.h"

#include "core/commands/arrangement/ArrangementCommands.h"

#include <algorithm>
#include <string_view>

namespace core::commands::arrangement
{
namespace
{
bool startsWith(std::string_view s, std::string_view p) noexcept
{
    return s.substr(0, p.size()) == p;
}

// "[<digits>]" at the front of s: consumed.
bool index(std::string_view& s) noexcept
{
    if (s.empty() || s.front() != '[')
        return false;
    std::size_t i = 1;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9')
        ++i;
    if (i == 1 || i >= s.size() || s[i] != ']')
        return false;
    s.remove_prefix(i + 1);
    return true;
}

bool planPath(std::string_view s) noexcept
{
    if (s == "arrangement" || s == kSectionsPath)
        return true;
    if (! startsWith(s, "scene"))
        return false;
    s.remove_prefix(5);
    if (! index(s))
        return false;
    if (s.empty())
        return true; // a whole scene (paste, clear, reset)
    if (! startsWith(s, ".pattern"))
        return false;
    s.remove_prefix(8);
    if (! index(s))
        return false;
    return s.empty() || s == ".length";
}
} // namespace

void buildPlan(const model::Project& p, core::arrangement::ArrangementPlan& out) noexcept
{
    const int count = rowCount(p);
    out.count = static_cast<std::uint16_t>(count);
    std::int64_t at = 0;
    for (int i = 0; i < count; ++i)
    {
        const model::Section s = normalised(p.arrangement.sections[static_cast<std::size_t>(i)]);
        auto& r = out.rows[static_cast<std::size_t>(i)];
        r.scene = s.scene;
        r.pattern = s.pattern;
        r.repeats = static_cast<std::uint8_t>(s.repeats);
        r.bpm = s.fixedBpm ? static_cast<float>(*s.fixedBpm) : 0.0f;
        const int length = std::clamp(p.scenes[s.scene].patterns[s.pattern].length, 1, 64);
        out.startPpq16[static_cast<std::size_t>(i)] = at;
        at += static_cast<std::int64_t>(s.repeats) * length;
    }
    out.startPpq16[static_cast<std::size_t>(count)] = at;
}

bool affectsPlan(const ChangeSet& c) noexcept
{
    return std::any_of(c.paths().begin(), c.paths().end(), [](const std::string& s) { return planPath(s); });
}

bool affectsPlay(const ChangeSet& c) noexcept
{
    return std::any_of(c.paths().begin(), c.paths().end(), [](const std::string& s) { return s == kPlayPath; });
}
} // namespace core::commands::arrangement
