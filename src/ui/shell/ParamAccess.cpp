#include "ui/shell/ParamAccess.h"

#include <algorithm>

namespace ui
{
namespace
{
constexpr std::size_t kTrackSlots = 256, kSceneSlots = 16, kMaxLocks = 1024;
}

std::size_t LocalParamValues::slots(const core::ParamSpec& s) noexcept
{
    switch (s.scope)
    {
    case core::ParamScope::Track:
        return kTrackSlots;
    case core::ParamScope::Scene:
    case core::ParamScope::SceneOverrideElseProject:
        return kSceneSlots;
    default:
        return 1;
    }
}

LocalParamValues::LocalParamValues(const core::ParamRegistry& params, engine::EngineBridge* bridge)
    : params_(params), bridge_(bridge)
{
    std::size_t total = 0;
    for (const auto& s : params_.all())
    {
        offset_.push_back(total);
        total += slots(s);
    }
    values_.resize(total);
    const auto all = params_.all();
    for (std::size_t i = 0; i < all.size(); ++i)
        std::fill_n(values_.begin() + static_cast<std::ptrdiff_t>(offset_[i]), slots(all[i]), all[i].defaultValue);
    // TRK-031: the model's per-track default PAD COLOR is the track's own palette index (makeDefaultProject); a
    // ParamSpec has one default, so the stand-in for the model seeds it per track scope (scope & 15 = track).
    if (const auto* colour = params_.find("track.padColor"); colour != nullptr && slots(*colour) == kTrackSlots)
    {
        const auto i = indexOf(*colour);
        for (std::size_t scope = 0; scope < kTrackSlots; ++scope)
            values_[offset_[i] + scope] = static_cast<float>(scope & 15);
    }
    locks_.reserve(kMaxLocks);
}

std::size_t LocalParamValues::indexOf(const core::ParamSpec& s) const noexcept
{
    const auto all = params_.all();
    return static_cast<std::size_t>(&s - all.data());
}

float LocalParamValues::get(const core::ParamSpec& s, std::uint16_t scope) const
{
    const auto i = indexOf(s);
    if (i >= offset_.size())
        return s.defaultValue;
    return values_[offset_[i] + (scope % slots(s))];
}

void LocalParamValues::set(const core::ParamSpec& s, std::uint16_t scope, float value)
{
    const auto i = indexOf(s);
    if (i >= offset_.size())
        return;
    const float v = s.clamp(value);
    values_[offset_[i] + (scope % slots(s))] = v;
    if (bridge_ != nullptr)
        if (const auto id = params_.idOf(s.id); id && id->valid())
            bridge_->post(engine::ParamChange{*id, scope, v, engine::kBlockFrames});
}

bool LocalParamValues::lockOf(const core::ParamSpec& s, std::uint16_t scope, int step, float& value) const
{
    for (const auto& l : locks_)
        if (l.spec == &s && l.scope == scope && l.step == step)
        {
            value = l.value;
            return true;
        }
    return false;
}

void LocalParamValues::setLock(const core::ParamSpec& s, std::uint16_t scope, int step, float value)
{
    for (auto& l : locks_)
        if (l.spec == &s && l.scope == scope && l.step == step)
        {
            l.value = s.clamp(value);
            return;
        }
    if (locks_.size() < kMaxLocks)
        locks_.push_back({&s, scope, static_cast<std::uint8_t>(step), s.clamp(value)});
}
} // namespace ui
