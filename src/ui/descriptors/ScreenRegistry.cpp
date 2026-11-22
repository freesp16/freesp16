// ScreenRegistry and the descriptor validator (UID-205…207, UID-225). Run at start-up and in
// tests/unit/ui/test_screen_registry.cpp. Checks, one message per problem:
//   - ids: present and unique; a second descriptor with the same id must carry a featureFlag (an extension variant)
//   - navigation: the parent exists, the parent chain reaches the root without a cycle (breadcrumbs, UID-206/300),
//     every rail `opens` names a registered screen or popup
//   - rail: ≤ 4 items (5 = the compact rail of a rail template, 03-layout-grid.md); `binds` names a ParamSpec
//   - strip: every column is a registered ParamSpec
//   - body: the factory builds the body component
//   - UID-225: a feature-flagged variant of a reference screen may only fill extension slots — its reference rail
//     items stay first and unchanged (it may append up to the rail's limit), reference strip columns stay where they
//     are (it may fill empty columns), and template, context bar, parent and body component are unchanged. Anything
//     else would move a reference control and is rejected.
#include "ui/descriptors/ScreenSpec.h"

#include "core/params/ParamRegistry.h"
#include "ui/descriptors/PopupSpec.h"
#include "ui/widgets/Widget.h"

namespace ui
{
namespace
{
bool sameItem(const RailItemSpec& a, const RailItemSpec& b) noexcept
{
    return a.kind == b.kind && a.label == b.label && a.opens == b.opens && a.binds == b.binds;
}

constexpr std::size_t kRailMax = 4, kRailCompactMax = 5, kMaxDepth = 8;
} // namespace

ScreenRegistry& ScreenRegistry::builtin() noexcept
{
    static ScreenRegistry registry;
    return registry;
}

void ScreenRegistry::add(const ScreenSpec& spec)
{
    screens_.push_back(spec);
}

// The reference descriptor of an id (the one without a feature flag), else the first variant.
const ScreenSpec* ScreenRegistry::find(std::string_view id) const
{
    for (const auto& s : screens_)
        if (s.id == id && s.featureFlag.empty())
            return &s;
    for (const auto& s : screens_)
        if (s.id == id)
            return &s;
    return nullptr;
}

std::vector<std::string> ScreenRegistry::validate(const WidgetFactory& factory, const core::ParamRegistry& params) const
{
    std::vector<std::string> problems;
    auto problem = [&](const ScreenSpec& s, const std::string& what)
    {
        problems.push_back(std::string(s.id) + (s.featureFlag.empty() ? "" : " [" + std::string(s.featureFlag) + "]") +
                           ": " + what);
    };
    const auto& popups = PopupRegistry::builtin();
    for (std::size_t i = 0; i < screens_.size(); ++i)
    {
        const auto& s = screens_[i];
        if (s.id.empty())
            problems.push_back("screen without id");
        if (s.title.empty())
            problem(s, "no title (breadcrumb segment)");
        const ScreenSpec* reference = nullptr;
        for (std::size_t j = 0; j < screens_.size(); ++j)
            if (j != i && screens_[j].id == s.id)
            {
                if (s.featureFlag.empty() && screens_[j].featureFlag.empty() && j < i)
                    problem(s, "duplicate id");
                if (screens_[j].featureFlag.empty())
                    reference = &screens_[j];
            }
        if (! s.parent.empty() && find(s.parent) == nullptr)
            problem(s, "unknown parent " + std::string(s.parent));
        {
            const ScreenSpec* c = &s;
            std::size_t depth = 0;
            while (c != nullptr && ! c->parent.empty() && depth < kMaxDepth)
            {
                c = find(c->parent);
                ++depth;
            }
            if (depth >= kMaxDepth)
                problem(s, "parent chain deeper than 8 or cyclic");
        }
        const bool railTemplate = s.body == BodyTemplate::RailContent || s.body == BodyTemplate::RailContentMeter ||
                                  s.body == BodyTemplate::RailContentMeters2 ||
                                  s.body == BodyTemplate::RailMeterContent;
        if (s.rail.size() > (railTemplate ? kRailCompactMax : kRailMax))
            problem(s, "more than 4 rail items (5 with the compact rail of a rail template)");
        for (const auto& item : s.rail)
        {
            if (item.label.empty() && (! item.opens.empty() || ! item.binds.empty()))
                problem(s, "rail hole (empty label) with a target: a hole is never focused or hit (RailOwner.h)");
            if (! item.opens.empty() && find(item.opens) == nullptr && popups.find(item.opens) == nullptr)
                problem(s, "rail item " + std::string(item.label) + " opens unknown screen or popup " +
                               std::string(item.opens));
            if (! item.binds.empty() && params.find(item.binds) == nullptr)
                problem(s,
                        "rail item " + std::string(item.label) + " binds unknown parameter " + std::string(item.binds));
        }
        for (const auto& column : s.strip.columns)
            if (! column.empty() && params.find(column) == nullptr)
                problem(s, "strip column " + std::string(column) + " is not a registered ParamSpec");
        if (! s.bodyComponent.empty() && factory && ! factory(s, WidgetArgs{}))
            problem(s, "body component " + std::string(s.bodyComponent) + " is not registered");

        // UID-225: a flagged variant of a reference screen only fills extension slots.
        if (! s.featureFlag.empty() && reference != nullptr)
        {
            const auto& r = *reference;
            if (s.body != r.body || s.contextBar != r.contextBar || s.parent != r.parent ||
                s.bodyComponent != r.bodyComponent)
                problem(s, "changes the reference template, context bar, parent or body (UID-225)");
            const std::size_t limit = r.rail.size() > kRailMax ? kRailCompactMax : kRailMax;
            if (s.rail.size() < r.rail.size() || s.rail.size() > limit)
                problem(s, "removes reference rail items or exceeds the rail (UID-225)");
            for (std::size_t k = 0; k < r.rail.size() && k < s.rail.size(); ++k)
                if (! sameItem(r.rail[k], s.rail[k]))
                    problem(s, "moves reference rail item " + std::string(r.rail[k].label) + " (UID-225)");
            for (std::size_t n = 0; n < 6; ++n)
                if (! r.strip.columns[n].empty() && s.strip.columns[n] != r.strip.columns[n])
                    problem(s, "moves reference strip column " + std::to_string(n + 1) + " (" +
                                   std::string(r.strip.columns[n]) + ") (UID-225)");
            if (s.strip.tab != r.strip.tab || s.strip.groupMask != r.strip.groupMask)
                problem(s, "changes the reference strip tab or groups (UID-225)");
        }
    }
    return problems;
}
} // namespace ui
