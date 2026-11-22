#include "core/commands/ribbon/RibbonCommands.h"

#include <algorithm>
#include <cctype>

namespace core::commands::ribbon
{
namespace
{
// The whole RibbonSettings after the edit; `key` names what a knob edit changes ("" = never coalesces).
class SetRibbon final : public Command
{
public:
    SetRibbon(model::RibbonSettings value, std::string_view label, std::string key)
        : value_(std::move(value)), label_(label), key_(std::move(key))
    {
    }
    std::string_view label() const override { return label_; }
    void apply(model::Project& p) override { p.ribbon = value_; }
    std::unique_ptr<Command> inverse(const model::Project& before) const override
    {
        return std::make_unique<SetRibbon>(before.ribbon, label_, key_);
    }
    bool mergeWith(const Command& other, double) override
    {
        const auto* o = dynamic_cast<const SetRibbon*>(&other);
        return o != nullptr && ! key_.empty() && o->key_ == key_;
    }
    void collectChanges(ChangeSet& c) const override { c.add(std::string(kPath)); }
    std::size_t sizeBytes() const override
    {
        std::size_t n = sizeof(*this);
        for (const auto& set : value_.user)
            for (const auto& it : set)
                n += it.target.capacity();
        return n;
    }

private:
    model::RibbonSettings value_;
    std::string_view label_;
    std::string key_;
};

bool validSlot(int set, int slot) noexcept { return set >= 0 && set < kSets && slot >= 0 && slot < kItems; }
double clamp01(double v) noexcept { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
std::string slotKey(const char* what, int set, int slot)
{
    return std::string(what) + "[" + std::to_string(set) + "][" + std::to_string(slot) + "]";
}
} // namespace

int itemCount(const model::RibbonSettings& r, int set) noexcept
{
    if (set < 0 || set >= kSets)
        return 0;
    int n = 0;
    while (n < kItems && ! r.user[static_cast<std::size_t>(set)][static_cast<std::size_t>(n)].target.empty())
        ++n;
    return n;
}

bool isStripTarget(const ParamRegistry& registry, std::string_view id) noexcept
{
    const auto* spec = registry.find(id);
    return spec != nullptr && spec->stripTarget;
}

std::string targetName(const ParamSpec& spec)
{
    std::string name(spec.shortLabel.empty() ? spec.label : spec.shortLabel);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (const auto at = name.find("MIXRATIO"); at != std::string::npos)
        name.replace(at, 8, "MIX");
    const std::string_view id = spec.id;
    if (id.substr(0, 4) == "fx1." || id.substr(0, 4) == "fx2.")
        name = (id[2] == '1' ? "FX1 " : "FX2 ") + name;
    return name;
}

std::unique_ptr<Command> assignItem(const model::Project& p, int set, int slot, std::string_view targetId,
                                    const ParamRegistry& registry)
{
    if (! validSlot(set, slot) || ! isStripTarget(registry, targetId))
        return nullptr;
    auto r = p.ribbon;
    const int count = itemCount(r, set);
    if (slot > count)
        slot = count; // appended: the list stays contiguous
    if (slot >= kItems)
        return nullptr;
    auto& item = r.user[static_cast<std::size_t>(set)][static_cast<std::size_t>(slot)];
    if (item.target == targetId)
        return nullptr;
    item = model::RibbonItem {std::string(targetId), 1.0, 0.0};
    return std::make_unique<SetRibbon>(std::move(r), "ASSIGN", std::string());
}

std::unique_ptr<Command> setItemRange(const model::Project& p, int set, int slot, double bottom01, double top01)
{
    if (! validSlot(set, slot))
        return nullptr;
    auto r = p.ribbon;
    auto& item = r.user[static_cast<std::size_t>(set)][static_cast<std::size_t>(slot)];
    if (item.target.empty())
        return nullptr;
    const double b = clamp01(bottom01), t = clamp01(top01);
    if (item.bottom == b && item.top == t)
        return nullptr;
    item.bottom = b;
    item.top = t;
    return std::make_unique<SetRibbon>(std::move(r), "STRIP VALUE", slotKey("range", set, slot));
}

std::unique_ptr<Command> deleteItem(const model::Project& p, int set, int slot)
{
    if (! validSlot(set, slot))
        return nullptr;
    auto r = p.ribbon;
    auto& items = r.user[static_cast<std::size_t>(set)];
    if (items[static_cast<std::size_t>(slot)].target.empty())
        return nullptr;
    for (int i = slot; i + 1 < kItems; ++i)
        items[static_cast<std::size_t>(i)] = std::move(items[static_cast<std::size_t>(i + 1)]);
    items[kItems - 1] = model::RibbonItem {};
    return std::make_unique<SetRibbon>(std::move(r), "DELETE", std::string());
}

std::unique_ptr<Command> setPitchRange(const model::Project& p, int bottomCents, int topCents)
{
    auto r = p.ribbon;
    r.pitchBottomCents = std::clamp(bottomCents, -2400, 0);
    r.pitchTopCents = std::clamp(topCents, 0, 2400);
    if (r.pitchBottomCents == p.ribbon.pitchBottomCents && r.pitchTopCents == p.ribbon.pitchTopCents)
        return nullptr;
    return std::make_unique<SetRibbon>(std::move(r), "PITCH RANGE", "pitch");
}

std::unique_ptr<Command> replaceRibbon(model::RibbonSettings settings, std::string_view label)
{
    return std::make_unique<SetRibbon>(std::move(settings), label, std::string());
}
} // namespace core::commands::ribbon
