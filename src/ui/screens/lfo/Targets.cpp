#include "ui/screens/lfo/Targets.h"

#include "core/commands/ribbon/RibbonCommands.h"
#include "engine/fx/EffectRegistry.h"
#include "ui/screens/effects/FxSlots.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <deque>
#include <string>
#include <unordered_map>

namespace ui::m4
{
namespace
{
// Group rank in the module chain (PLAYBACK, AMP ENVELOPE, FX1, FX2, MIXER, then the rest in registry order).
int rankOf(std::string_view group) noexcept
{
    if (group == "PLAYBACK")
        return 0;
    if (group == "AMP ENVELOPE")
        return 1;
    if (group == "FX1")
        return 2;
    if (group == "FX2")
        return 3;
    if (group == "TRACK" || group == "MIXER")
        return 4;
    return 5;
}

std::string upper(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

std::string_view effectName(std::string_view effectId)
{
    const auto* d = ui::fx::environment().effectRegistry().find(effectId);
    return d != nullptr && ! d->name.empty() ? d->name : effectId;
}

std::deque<std::string>& groupPool(TargetKind k)
{
    static std::array<std::deque<std::string>, 2> pools;
    return pools[static_cast<std::size_t>(k)];
}
} // namespace

std::string_view fxEffectOf(std::string_view id, int& slot) noexcept
{
    slot = -1;
    if (id.size() < 5 || (id.substr(0, 4) != "fx1." && id.substr(0, 4) != "fx2."))
        return {};
    const auto rest = id.substr(4);
    const auto dot = rest.find('.');
    if (dot == std::string_view::npos)
        return {};
    slot = id[2] == '1' ? 0 : 1;
    return rest.substr(0, dot);
}

void buildTargets(const core::ParamRegistry& reg, TargetKind kind, int scene0, int track0, std::vector<ListItem>& out)
{
    out.clear();
    auto& pool = groupPool(kind);
    pool.clear();
    out.push_back({"NONE", {}, {}, -1});
    const auto ids = kind == TargetKind::Lfo ? reg.lfoTargets() : reg.stripTargets();
    std::string_view inSlot[2];
    auto& slots = ui::fx::environment().slotAccess();
    inSlot[0] = slots.effect(ui::fx::Slot::Fx1, scene0 & 15, track0 & 15);
    inSlot[1] = slots.effect(ui::fx::Slot::Fx2, scene0 & 15, track0 & 15);
    std::string_view fxGroup[2];
    for (int s = 0; s < 2; ++s)
        if (! inSlot[s].empty() && inSlot[s] != "none")
        {
            pool.push_back(std::string(s == 0 ? "FX1" : "FX2") + (kind == TargetKind::Lfo ? " : " : " \xC2\xB7 ") +
                           upper(effectName(inSlot[s])));
            fxGroup[s] = pool.back();
        }
    for (int rank = 0; rank <= 5; ++rank)
        for (const auto id : ids)
        {
            const auto& spec = reg[id];
            if (rankOf(spec.group) != rank || ! spec.featureFlag.empty())
                continue;
            std::string_view group = spec.group;
            int slot = -1;
            const auto effect = fxEffectOf(spec.id, slot);
            if (slot >= 0)
            {
                if (effect != inSlot[slot] || spec.id.substr(spec.id.size() - 4) == ".div")
                    continue; // FX-083: only the effect in the slot; its SYNC twins are the same knob
                group = fxGroup[slot];
            }
            else if (rank == 4)
                group = "MIXER";
            out.push_back({spec.label, group, {}, static_cast<int>(id.value)});
        }
}

std::string_view targetName(const core::ParamRegistry& reg, core::ParamId id)
{
    if (! id.valid() || id.value >= reg.size())
        return "NONE";
    static std::unordered_map<std::uint16_t, std::string> names;
    auto it = names.find(id.value);
    if (it == names.end())
        it = names.emplace(id.value, core::commands::ribbon::targetName(reg[id])).first;
    return it->second;
}

std::string_view targetPath(const core::ParamRegistry& reg, core::ParamId id)
{
    if (! id.valid() || id.value >= reg.size())
        return {};
    static std::unordered_map<std::uint16_t, std::string> paths;
    auto it = paths.find(id.value);
    if (it == paths.end())
    {
        const auto& spec = reg[id];
        int slot = -1;
        const auto effect = fxEffectOf(spec.id, slot);
        const std::string_view module = slot >= 0 ? (slot == 0 ? "FX1" : "FX2")
                                                  : (rankOf(spec.group) == 4 ? std::string_view("MIXER") : spec.group);
        // FX targets name their effect (utility.md: "FX1 › FILTER CUT OFF"), by its id upper-cased.
        it = paths.emplace(id.value, std::string(module) + " \xE2\x80\xBA " +
                                         (slot >= 0 ? upper(effect) + " " : std::string()) + upper(spec.label))
                 .first;
    }
    return it->second;
}

bool targetStale(const core::ParamRegistry& reg, core::ParamId id, int scene0, int track0)
{
    if (! id.valid() || id.value >= reg.size())
        return false;
    int slot = -1;
    const auto effect = fxEffectOf(reg[id].id, slot);
    if (slot < 0)
        return false;
    return ui::fx::environment().slotAccess().effect(slot == 0 ? ui::fx::Slot::Fx1 : ui::fx::Slot::Fx2, scene0 & 15,
                                                    track0 & 15) != effect;
}
} // namespace ui::m4
