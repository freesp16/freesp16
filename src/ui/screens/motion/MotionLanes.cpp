#include "ui/screens/motion/MotionLanes.h"

#include "core/commands/sequencer/SequencerCommands.h"
#include "core/model/Project.h"
#include "ui/screens/effects/FxCatalogue.h"
#include "ui/screens/effects/FxSlots.h"
#include "ui/screens/home/TrackInfo.h"
#include "ui/screens/sequence/M2Session.h"
#include "ui/shell/ShellApi.h"
#include "ui/shell/UiState.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace ui::motion
{
namespace
{
constexpr int kTrackSlots = 23, kFx1First = 23, kFx2First = 29;

// The ParamSpecs of the track lock slots 0–22, for one registry (looked up once).
const core::ParamSpec* trackSlotSpec(const core::ParamRegistry& params, int slot)
{
    static const core::ParamRegistry* builtFor = nullptr;
    static std::array<const core::ParamSpec*, kTrackSlots> table {};
    if (builtFor != &params)
    {
        table.fill(nullptr);
        for (const auto& spec : params.all())
        {
            const int s = spec.lockable ? core::commands::sequencer::lockSlotOf(spec, params) : -1;
            if (s >= 0 && s < kTrackSlots && table[static_cast<std::size_t>(s)] == nullptr)
                table[static_cast<std::size_t>(s)] = &spec;
        }
        builtFor = &params;
    }
    return slot >= 0 && slot < kTrackSlots ? table[static_cast<std::size_t>(slot)] : nullptr;
}
} // namespace

const core::ParamSpec* specOf(ShellApi& shell, int slot)
{
    const auto& params = shell.params();
    if (slot < kTrackSlots)
        return trackSlotSpec(params, slot);
    if (slot >= core::model::kLockSlots)
        return nullptr;
    const auto fxSlot = slot < kFx2First ? fx::Slot::Fx1 : fx::Slot::Fx2;
    const auto& st = shell.state();
    const auto& env = fx::environment();
    const auto effect = env.slotAccess().effect(fxSlot, st.scene, st.selectedTrack);
    if (fx::isNone(effect))
        return nullptr;
    const auto* d = fx::findEffect(env.effectRegistry(), effect);
    if (d == nullptr)
        return nullptr;
    for (const auto& id : d->params)
        if (const auto* spec = id.empty() ? nullptr : fx::resolveParam(params, fxSlot, *d, id))
            if (core::commands::sequencer::lockSlotOf(*spec, params) == slot)
                return spec;
    return nullptr;
}

std::uint16_t scopeOf(ShellApi& shell, const core::ParamSpec& spec)
{
    const auto& st = shell.state();
    if (spec.id.substr(0, 4) == "fx1." || spec.id.substr(0, 4) == "fx2.")
        return fx::scopeOf(spec, st.scene, st.selectedTrack);
    return m1::scopeFor(spec, st, st.selectedTrack);
}

const core::model::Pattern* pattern(ShellApi& shell)
{
    const auto& st = shell.state();
    const auto& p = m2::Session::instance().project();
    return &p.scenes[static_cast<std::size_t>((st.scene - 1) & 15)].patterns[static_cast<std::size_t>((st.pattern - 1) & 15)];
}

int patternLength(ShellApi& shell)
{
    const auto* p = pattern(shell);
    return p != nullptr ? std::clamp(p->length, 1, core::model::kSteps) : 16;
}

std::uint64_t recordedSlots(const core::model::Pattern& p, int track0)
{
    std::uint64_t mask = 0;
    for (int s = 0; s < core::model::kSteps; ++s)
    {
        const auto& c = p.steps[static_cast<std::size_t>(s)][static_cast<std::size_t>(track0 & 15)];
        if (c.lockIndex != core::model::Step::kNoLocks && c.lockIndex < p.locks.size())
            mask |= p.locks[c.lockIndex].mask;
    }
    return mask & ((std::uint64_t {1} << core::model::kLockSlots) - 1);
}

int stepsWithMotion(const core::model::Pattern& p, int track0, int length)
{
    int n = 0;
    for (int s = 0; s < std::min(length, core::model::kSteps); ++s)
    {
        const auto& c = p.steps[static_cast<std::size_t>(s)][static_cast<std::size_t>(track0 & 15)];
        if (c.lockIndex != core::model::Step::kNoLocks && c.lockIndex < p.locks.size() && p.locks[c.lockIndex].mask != 0)
            ++n;
    }
    return n;
}

std::uint64_t lanes(ShellApi& shell)
{
    const auto* p = pattern(shell);
    return p != nullptr ? recordedSlots(*p, shell.state().selectedTrack - 1) : 0;
}

float valueAt(const core::ParamSpec& spec, float y01) noexcept
{
    const float v = spec.minValue + std::clamp(y01, 0.0f, 1.0f) * (spec.maxValue - spec.minValue);
    return spec.type == core::ParamType::Continuous ? spec.clamp(v) : spec.clamp(std::round(v));
}
} // namespace ui::motion
