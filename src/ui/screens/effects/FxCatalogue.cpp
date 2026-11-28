#include "ui/screens/effects/FxCatalogue.h"

#include "engine/fx/EffectRegistryOrder.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace ui::fx
{
namespace
{
// UID-831 [OBS] 1.60 (screens/effects.md "SELECT FX"); "delay" is the insert delay, "delayBus" the Send/Master delay
// (both labelled DELAY). Ours, after the 1.60 grid: REVERB on the track inserts, then "vpitch" (VINTAGE PITCH) last
// in both grids.
constexpr std::string_view kInsertOrder[] = {"none",       "chorus",     "flanger", "eq2",    "filter",
                                             "delay",      "lofi",       "compressor", "distortion", "phaser",
                                             "ducker",     "reverb",     "vpitch"};
constexpr std::string_view kBusOrder[] = {"none",   "delayBus", "delay",      "reverb",     "chorus", "flanger", "eq2",
                                          "filter", "lofi",     "compressor", "distortion", "phaser", "vpitch"};
constexpr std::string_view kExtensions[] = {"limiter", "ringmod",    "tremolo", "autopan",
                                            "autowah", "vinylbrake", "trans",   "roll"};

const engine::fx::EffectDescriptor kNoneDescriptor{
    .id = kNone,
    .name = "NONE",
    .slots = static_cast<std::uint8_t>(engine::fx::SlotKind::Insert | engine::fx::SlotKind::Send) |
             static_cast<std::uint8_t>(engine::fx::SlotKind::Master),
    .params = {},
    .options = {},
    .visual = {},
    .shell = {},
    .create = nullptr};

// Concatenate the parts into buf, truncating to its capacity.
std::string_view join(char* buf, std::size_t n, std::string_view a, std::string_view b, std::string_view c = {},
                      std::string_view d = {}) noexcept
{
    std::size_t at = 0;
    for (auto part : {a, b, c, d})
    {
        const auto k = std::min(part.size(), n - at);
        if (k != 0)
        {
            std::memcpy(buf + at, part.data(), k);
            at += k;
        }
    }
    return {buf, at};
}

int rank(std::span<const std::string_view> order, std::string_view id) noexcept
{
    for (std::size_t i = 0; i < order.size(); ++i)
        if (order[i] == id)
            return static_cast<int>(i);
    return -1;
}

// NONE (Off, FX-091): a registered unflagged "none" descriptor, else the synthesised one. The engine's own "none"
// test effect (flag fx.test, a pass-through with MIX) is never the UI's NONE.
const engine::fx::EffectDescriptor& findNoneDescriptor(const engine::fx::EffectRegistry& registry) noexcept
{
    const auto* d = registry.find(kNone);
    return d != nullptr && d->featureFlag.empty() ? *d : kNoneDescriptor;
}

// The last dotted segment of an id ("fx.filter.cutoff" -> "cutoff").
std::string_view keyOf(std::string_view id) noexcept
{
    const auto dot = id.rfind('.');
    return dot == std::string_view::npos ? id : id.substr(dot + 1);
}
} // namespace

std::span<const std::string_view> referenceOrder(engine::fx::SlotKind k) noexcept
{
    if (k == engine::fx::SlotKind::Insert)
        return kInsertOrder;
    return kBusOrder;
}

std::span<const std::string_view> extensionOrder() noexcept
{
    return kExtensions;
}

const engine::fx::EffectDescriptor& noneDescriptor() noexcept
{
    return kNoneDescriptor;
}

bool listed(const engine::fx::EffectDescriptor& d, const FeatureFlags& flags) noexcept
{
    if (d.featureFlag.empty())
        return true;
    return flags.enabled(d.featureFlag) || (flags.enabled(kExtensionsFlag) && rank(kExtensions, d.id) >= 0);
}

void listFor(const engine::fx::EffectRegistry& registry, engine::fx::SlotKind k, const FeatureFlags& flags,
             std::vector<const engine::fx::EffectDescriptor*>& out)
{
    // Membership is FXCORE's (engine::fx::listFor: the slot mask, feature flags) with the flags `flags`
    // enables; the order is SELECT FX's (UID-831): the reference ids, the P3 extensions, then the rest in id order.
    std::vector<std::string_view> on;
    for (const auto& d : registry.all())
        if (! d.featureFlag.empty() && listed(d, flags) && std::find(on.begin(), on.end(), d.featureFlag) == on.end())
            on.push_back(d.featureFlag);
    auto members = engine::fx::listFor(registry, k, on);
    const auto order = ui::fx::referenceOrder(k); // not ADL's engine::fx::referenceOrder (FX-040 processor order)
    auto key = [&](const engine::fx::EffectDescriptor* d) {
        if (const int r = rank(order, d->id); r >= 0)
            return r;
        if (const int e = rank(kExtensions, d->id); e >= 0)
            return 100 + e;
        return 1000; // engine::fx::listFor keeps these in id order (stable sort below)
    };
    members.erase(std::remove_if(members.begin(), members.end(),
                                 [&](const auto* d) { return d->id == kNone || ! listed(*d, flags); }),
                  members.end());
    std::stable_sort(members.begin(), members.end(), [&](const auto* a, const auto* b) { return key(a) < key(b); });
    out.clear();
    out.push_back(&findNoneDescriptor(registry)); // NONE is always the first cell
    out.insert(out.end(), members.begin(), members.end());
}

const engine::fx::EffectDescriptor* findEffect(const engine::fx::EffectRegistry& registry, std::string_view id) noexcept
{
    if (isNone(id))
        return &findNoneDescriptor(registry);
    return registry.find(id);
}

const core::ParamSpec* resolveParam(const core::ParamRegistry& params, Slot slot, const engine::fx::EffectDescriptor& d,
                                    std::string_view param) noexcept
{
    if (param.empty())
        return nullptr;
    char buf[128], head[64];
    const auto p = prefix(slot);
    std::string_view candidate;
    if (param.substr(0, 3) == "fx.")
        candidate = join(buf, sizeof buf, p, ".", param.substr(3)); // fx.filter.cutoff -> fx1.filter.cutoff
    else if (param.find('.') == std::string_view::npos)
        candidate =
            join(buf, sizeof buf, join(head, sizeof head, p, ".", d.id, "."), param); // cutoff -> fx1.filter.cutoff
    else
        candidate = join(buf, sizeof buf, p, ".", param); // filter.cutoff -> fx1.filter.cutoff
    if (const auto* s = params.find(candidate))
        return s;
    return params.find(param); // registered once for every slot
}

const core::ParamSpec* syncForm(const core::ParamRegistry& params, const core::ParamSpec& spec) noexcept
{
    char buf[128];
    return params.find(join(buf, sizeof buf, spec.id, ".div"));
}

const core::ParamSpec* syncOption(const core::ParamRegistry& params, Slot slot,
                                  const engine::fx::EffectDescriptor& d) noexcept
{
    for (const auto option : d.options)
        if (keyOf(option) == "sync")
            if (const auto* s = resolveParam(params, slot, d, option); s != nullptr && s->type == core::ParamType::Bool)
                return s;
    return nullptr;
}

std::uint16_t scopeOf(const core::ParamSpec& s, int scene, int track) noexcept
{
    switch (s.scope)
    {
    case core::ParamScope::Track:
        return static_cast<std::uint16_t>(((scene - 1) & 15) << 4 | ((track - 1) & 15));
    case core::ParamScope::Scene:
    case core::ParamScope::SceneOverrideElseProject:
        return static_cast<std::uint16_t>(scene - 1);
    default:
        return 0;
    }
}
} // namespace ui::fx
