// Reference slot orders and registry checks — see EffectRegistryOrder.h. Message thread only.
#include "engine/fx/EffectRegistryOrder.h"

#include <algorithm>
#include <iterator>

namespace engine::fx
{
namespace
{
// FX-040 (effect-shell.md, research F016/F019): the processors' effect arrays, index = reference type index.
// "reverb" (ours, not in the reference arrays) is appended after the reference types, so their indices stay put.
constexpr std::string_view kInsertOrder[] = {"chorus",     "flanger",    "eq2",    "filter", "delay", "lofi",
                                             "distortion", "compressor", "phaser", "ducker", "reverb"};
constexpr std::string_view kBusOrder[] = {"delayBus", "reverb", "chorus",     "flanger",    "eq2",
                                          "filter",   "lofi",   "compressor", "distortion", "phaser"};
constexpr std::string_view kInsertPrefixes[] = {"fx1", "fx2"};
constexpr std::string_view kSendPrefixes[] = {"send"};
constexpr std::string_view kMasterPrefixes[] = {"master"};

bool enabled(const EffectDescriptor& d, std::span<const std::string_view> flags) noexcept
{
    return d.featureFlag.empty() || std::find(flags.begin(), flags.end(), d.featureFlag) != flags.end();
}

void add(std::vector<Text>& e, Text msg)
{
    e.push_back(std::move(msg)); // rt-ok: message thread (registry checks)
}

void addAll(std::vector<const EffectDescriptor*>& v, const EffectDescriptor* d)
{
    v.push_back(d); // rt-ok: message thread (slot lists)
}

bool contains(std::span<const std::string_view> ids, std::string_view id) noexcept
{
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}
} // namespace

std::span<const std::string_view> referenceOrder(SlotKind kind) noexcept
{
    if (kind == SlotKind::Insert)
        return kInsertOrder;
    return kBusOrder;
}

std::span<const std::string_view> slotPrefixes(SlotKind kind) noexcept
{
    switch (kind)
    {
    case SlotKind::Insert:
        return kInsertPrefixes;
    case SlotKind::Send:
        return kSendPrefixes;
    case SlotKind::Master:
        return kMasterPrefixes;
    }
    return {};
}

std::vector<const EffectDescriptor*> listFor(const EffectRegistry& r, SlotKind kind,
                                             std::span<const std::string_view> flags)
{
    std::vector<const EffectDescriptor*> out;
    const auto ref = referenceOrder(kind);
    for (auto id : ref)
        if (const auto* d = r.find(id); d != nullptr && allows(d->slots, kind) && enabled(*d, flags))
            addAll(out, d);
    std::vector<const EffectDescriptor*> rest;
    for (const auto& d : r.all())
        if (allows(d.slots, kind) && enabled(d, flags) && ! contains(ref, d.id))
            addAll(rest, &d);
    std::sort(rest.begin(), rest.end(), [](const auto* a, const auto* b) { return a->id < b->id; });
    std::copy(rest.begin(), rest.end(), std::back_inserter(out));
    return out;
}

std::vector<std::string_view> allFeatureFlags(const EffectRegistry& r)
{
    std::vector<std::string_view> flags;
    for (const auto& d : r.all())
        if (! d.featureFlag.empty() && ! contains(flags, d.featureFlag))
            flags.push_back(d.featureFlag); // rt-ok: message thread (set-up)
    return flags;
}

std::vector<const EffectDescriptor*> engineTypes(const EffectRegistry& r, SlotKind kind)
{
    const auto flags = allFeatureFlags(r);
    return listFor(r, kind, flags);
}

int typeIndexOf(std::span<const EffectDescriptor* const> list, std::string_view id) noexcept
{
    for (std::size_t i = 0; i < list.size(); ++i)
        if (list[i]->id == id)
            return static_cast<int>(i);
    return -1;
}

Text slotParamId(std::string_view slotPrefix, std::string_view id)
{
    if (id.substr(0, 3) != "fx.")
        return {};
    Text s(slotPrefix);
    s += id.substr(2); // ".<effect>.<key>"
    return s;
}

std::vector<Text> validateDescriptor(const EffectDescriptor& d, std::span<const std::string_view> paramIds)
{
    std::vector<Text> e;
    const Text who = "effect '" + Text(d.id) + "': ";
    if (d.id.empty() || d.id.find('.') != std::string_view::npos || d.id.find(' ') != std::string_view::npos)
        add(e, who + "id must be non-empty, without '.' or spaces");
    if (d.name.empty())
        add(e, who + "empty name");
    if (d.slots == 0 || (d.slots & ~kAllSlotKinds) != 0)
        add(e, who + "slot mask must be a non-empty subset of Insert|Send|Master");
    if (d.create == nullptr)
        add(e, who + "no factory (create)");
    const Text own = "fx." + Text(d.id) + ".";
    auto check = [&](std::string_view pid, const char* what)
    {
        if (pid.empty())
            return;
        if (pid.substr(0, own.size()) != own)
        {
            add(e, who + what + " '" + Text(pid) + "' must be named " + own + "<key>");
            return;
        }
        for (auto kind : {SlotKind::Insert, SlotKind::Send, SlotKind::Master})
        {
            if (! allows(d.slots, kind))
                continue;
            for (auto prefix : slotPrefixes(kind))
            {
                const auto full = slotParamId(prefix, pid);
                if (! contains(paramIds, full))
                    add(e, who + what + " '" + Text(pid) + "': no ParamSpec '" + full + "'");
            }
        }
    };
    int knobs = 0;
    for (auto p : d.params)
    {
        knobs += p.empty() ? 0 : 1;
        check(p, "parameter");
    }
    if (knobs == 0 && d.options.empty())
        add(e, who + "no parameter and no option");
    if (d.options.size() > 8)
        add(e, who + "more than 8 options");
    for (auto o : d.options) // an option may exist on some slot kinds only (Compressor LOOK AHEAD: bus, OQ-15/UID-827)
    {
        bool found = false;
        for (auto kind : {SlotKind::Insert, SlotKind::Send, SlotKind::Master})
            if (allows(d.slots, kind))
                for (auto prefix : slotPrefixes(kind))
                    found = found || contains(paramIds, slotParamId(prefix, o));
        if (! found || o.substr(0, own.size()) != own)
            check(o, "option");
    }
    return e;
}

std::vector<Text> validate(const EffectRegistry& r, std::span<const std::string_view> paramIds)
{
    std::vector<Text> e;
    for (auto id : r.errors())
        add(e, "effect '" + Text(id) + "': duplicate or empty id (rejected)");
    for (const auto& d : r.all())
    {
        auto more = validateDescriptor(d, paramIds);
        std::copy(more.begin(), more.end(), std::back_inserter(e));
    }
    return e;
}
} // namespace engine::fx
