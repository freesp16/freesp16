// FxSlotRoutes — see FxSlotRoutes.h. Message thread only (builds the table); find() is the audio thread's lookup.
#include "engine/fx/FxSlotRoutes.h"

#include <algorithm>
#include <string>

namespace engine::fx
{
FxRoute FxSlotRoutes::parse(std::string_view id, std::span<const EffectDescriptor* const> insertTypes,
                            std::span<const EffectDescriptor* const> sendTypes,
                            std::span<const EffectDescriptor* const> masterTypes, std::int8_t k)
{
    FxRoute r;
    const auto dot = id.find('.');
    if (dot == std::string_view::npos)
        return r;
    const auto prefix = id.substr(0, dot);
    auto rest = id.substr(dot + 1);
    if (prefix == "fx")
    {
        if (rest == "mode")
            r.kind = FxRoute::Kind::Mode;
        else if (rest == "quirkProfile")
            r.kind = FxRoute::Kind::Quirk;
        else if (rest == "meteredSlot")
            r.kind = FxRoute::Kind::Meter;
        return r;
    }
    if (prefix == "fx1" || prefix == "fx2")
    {
        r.slotKind = SlotKind::Insert;
        r.unit = prefix == "fx2" ? 1 : 0;
    }
    else if (prefix == "send")
        r.slotKind = SlotKind::Send;
    else if (prefix == "master")
        r.slotKind = SlotKind::Master;
    else
        return r;
    if (rest == "type")
    {
        r.kind = FxRoute::Kind::SlotType;
        return r;
    }
    if (rest == "bypass")
    {
        r.kind = FxRoute::Kind::SlotBypass;
        return r;
    }
    // The SYNC form "<slot>.<effect>.<key>.div" addresses the same knob (UID-830).
    constexpr std::string_view kDiv = ".div";
    bool div = false;
    if (rest.size() > kDiv.size() && rest.substr(rest.size() - kDiv.size()) == kDiv)
    {
        rest = rest.substr(0, rest.size() - kDiv.size());
        div = true;
    }
    const auto dot2 = rest.find('.');
    if (dot2 == std::string_view::npos)
        return r;
    const auto effect = rest.substr(0, dot2);
    const auto list =
        r.slotKind == SlotKind::Insert ? insertTypes : (r.slotKind == SlotKind::Send ? sendTypes : masterTypes);
    int type = -1;
    for (std::size_t i = 0; i < list.size(); ++i)
        if (list[i]->id == effect)
            type = static_cast<int>(i);
    if (type < 0)
        return r;
    const auto& d = *list[static_cast<std::size_t>(type)];
    std::string pid = "fx."; // rt-ok: message thread, build()
    pid += rest;
    for (int pos = 0; pos < 6; ++pos)
        if (! d.params[static_cast<std::size_t>(pos)].empty() && d.params[static_cast<std::size_t>(pos)] == pid)
        {
            r.kind = FxRoute::Kind::Knob;
            r.type = static_cast<std::int8_t>(type);
            r.index = static_cast<std::int8_t>(k >= 0 && k < 6 ? k : pos);
            return r;
        }
    if (div)
        return FxRoute{}; // a .div of something that is not a knob
    for (std::size_t i = 0; i < d.options.size(); ++i)
        if (d.options[i] == pid)
        {
            r.kind = FxRoute::Kind::Option;
            r.type = static_cast<std::int8_t>(type);
            r.index = static_cast<std::int8_t>(i);
            return r;
        }
    return r;
}

void FxSlotRoutes::build(std::span<const EffectDescriptor* const> insertTypes,
                         std::span<const EffectDescriptor* const> sendTypes,
                         std::span<const EffectDescriptor* const> masterTypes,
                         std::span<const std::string_view> paramIds, std::span<const std::int8_t> internalIndex)
{
    routes_.assign(paramIds.size(), FxRoute{});
    for (std::size_t i = 0; i < paramIds.size(); ++i)
    {
        const std::int8_t k = i < internalIndex.size() ? internalIndex[i] : static_cast<std::int8_t>(-1);
        routes_[i] = parse(paramIds[i], insertTypes, sendTypes, masterTypes, k);
    }
}

std::size_t FxSlotRoutes::routed() const noexcept
{
    return static_cast<std::size_t>(
        std::count_if(routes_.begin(), routes_.end(), [](const FxRoute& r) { return r.kind != FxRoute::Kind::None; }));
}
} // namespace engine::fx
