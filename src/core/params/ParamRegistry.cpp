#include "core/params/ParamRegistry.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace core
{
float ParamSpec::step(float v, int detents, bool fine) const noexcept
{
    const float s = fine ? fineStep : coarseStep;
    float next = clamp(v + s * static_cast<float>(detents));
    if (type == ParamType::Enum || type == ParamType::Integer || type == ParamType::Bool)
        return std::round(next);
    // No drift: float steps of 0.1 and the like gather rounding error turn after turn, so a result within 1 % of a
    // step of the grid (minValue + k · the smaller step) lands exactly on it, the same float whatever path reached it.
    // The range's ends stay exact; a value off the grid (an import, an LFO-written base) stays where the turn puts it.
    if (next == minValue || next == maxValue)
        return next;
    // A step written as a short decimal is taken as that decimal (0.1f is 0.100000001…, and k × that would drift
    // again); any other step (1/127 …) as the float itself.
    const auto exact = [](float f) {
        const double d = std::round(static_cast<double>(f) * 1e6) / 1e6;
        return static_cast<float>(d) == f ? d : static_cast<double>(f);
    };
    const float grid = std::min(fineStep, coarseStep) > 0 ? std::min(fineStep, coarseStep) : s;
    if (grid > 0)
    {
        const double g = exact(grid), lo = exact(minValue);
        const double q = (static_cast<double>(next) - lo) / g, k = std::round(q);
        if (std::fabs(q - k) < 0.01)
            next = clamp(static_cast<float>(lo + k * g));
    }
    return next;
}

int ParamSpec::formatValue(float v, char* buf, int bufSize) const noexcept
{
    if (format != nullptr)
        return format(v, buf, bufSize);
    switch (type)
    {
        case ParamType::Enum:
        {
            const auto index = static_cast<std::size_t>(std::lround(clamp(v) - minValue));
            if (index < enumLabels.size())
                return std::snprintf(buf, static_cast<std::size_t>(bufSize), "%.*s",
                                     static_cast<int>(enumLabels[index].size()), enumLabels[index].data());
            return std::snprintf(buf, static_cast<std::size_t>(bufSize), "%ld", std::lround(v));
        }
        case ParamType::Bool: return std::snprintf(buf, static_cast<std::size_t>(bufSize), "%s", v >= 0.5f ? "ON" : "OFF");
        case ParamType::Integer:
        case ParamType::Note: return std::snprintf(buf, static_cast<std::size_t>(bufSize), "%ld", std::lround(v));
        case ParamType::Continuous:
        case ParamType::Time: break;
    }
    return std::snprintf(buf, static_cast<std::size_t>(bufSize), "%.1f", static_cast<double>(v));
}

ParamId ParamRegistry::add(const ParamSpec& spec)
{
    if (spec.id.empty() || spec.label.empty())
        errors_.push_back("ParamSpec without id or label");
    else if (find(spec.id) != nullptr)
        errors_.push_back(spec.id);
    else if (spec.type == ParamType::Enum && spec.enumLabels.size() != static_cast<std::size_t>(spec.maxValue - spec.minValue + 1))
        errors_.push_back(spec.id);
    else if (spec.minValue > spec.maxValue || spec.clamp(spec.defaultValue) != spec.defaultValue)
        errors_.push_back(spec.id);
    else
    {
        specs_.push_back(spec);
        return ParamId {static_cast<std::uint16_t>(specs_.size() - 1)};
    }
    return {};
}

const ParamSpec* ParamRegistry::find(std::string_view id) const noexcept
{
    for (const auto& s : specs_)
        if (s.id == id)
            return &s;
    return nullptr;
}

std::optional<ParamId> ParamRegistry::idOf(std::string_view id) const noexcept
{
    for (std::size_t i = 0; i < specs_.size(); ++i)
        if (specs_[i].id == id)
            return ParamId {static_cast<std::uint16_t>(i)};
    return std::nullopt;
}

namespace
{
template <typename Pred>
std::vector<ParamId> filter(const std::vector<ParamSpec>& specs, Pred pred)
{
    std::vector<ParamId> out;
    for (std::size_t i = 0; i < specs.size(); ++i)
        if (pred(specs[i]))
            out.push_back(ParamId {static_cast<std::uint16_t>(i)});
    return out;
}
} // namespace

std::vector<ParamId> ParamRegistry::lockable() const { return filter(specs_, [](auto& s) { return s.lockable; }); }
std::vector<ParamId> ParamRegistry::lfoTargets() const { return filter(specs_, [](auto& s) { return s.lfoTarget; }); }
std::vector<ParamId> ParamRegistry::stripTargets() const { return filter(specs_, [](auto& s) { return s.stripTarget; }); }

std::optional<ParamId> ParamRegistry::byMidiCc(int cc) const noexcept
{
    for (std::size_t i = 0; i < specs_.size(); ++i)
        if (specs_[i].midiCc == cc)
            return ParamId {static_cast<std::uint16_t>(i)};
    return std::nullopt;
}

std::vector<ParamId> ParamRegistry::byScope(ParamScope scope) const
{
    return filter(specs_, [scope](auto& s) { return s.scope == scope; });
}

namespace
{
std::vector<ParamCatalogue>& catalogueList()
{
    static std::vector<ParamCatalogue> list;
    return list;
}
} // namespace

void registerParamCatalogue(const ParamCatalogue& c) { catalogueList().push_back(c); }

std::vector<ParamCatalogue> paramCatalogues()
{
    auto list = catalogueList();
    std::sort(list.begin(), list.end(), [](const ParamCatalogue& a, const ParamCatalogue& b) {
        return a.order != b.order ? a.order < b.order : a.area < b.area;
    });
    return list;
}

void registerBuiltinParams(ParamRegistry& r)
{
    for (const auto& c : paramCatalogues())
        if (c.add != nullptr)
            c.add(r);
}

ParamRegistry& builtinParams()
{
    static ParamRegistry registry = [] {
        ParamRegistry r;
        registerBuiltinParams(r);
        return r;
    }();
    return registry;
}
} // namespace core
