#include "ui/input/ext/striprecord/StripRecorder.h"

#include "core/commands/sequencer/SequencerCommands.h"
#include "core/util/ribbon/Ribbon.h"

#include <cmath>

namespace ui::input::ext
{
namespace
{
bool integral(const core::ParamSpec& s) noexcept
{
    return s.type == core::ParamType::Integer || s.type == core::ParamType::Enum || s.type == core::ParamType::Bool ||
           s.type == core::ParamType::Note;
}
} // namespace

StripRecorder::StripRecorder(const core::ParamRegistry& registry)
    : registry_(registry), pitch_(registry.find("playback.pitch"))
{
}

void StripRecorder::reset() noexcept
{
    for (auto& t : last_)
        t.fill({});
    touching_ = false;
}

void StripRecorder::emit(const StripRecordState& s, const core::ParamSpec& spec, float value,
                         std::vector<LockWrite>& out)
{
    if (! spec.lockable || spec.scope != core::ParamScope::Track)
        return; // FX-094: Send / Master FX and preferences are no step-lock targets
    const int slot = core::commands::sequencer::lockSlotOf(spec, registry_);
    if (slot < 0 || slot >= core::model::kLockSlots)
        return;
    if (integral(spec))
        value = std::nearbyint(value);
    value = spec.clamp(value);
    for (int t = 0; t < core::model::kTracks; ++t)
    {
        if ((s.tracks >> t & 1u) == 0)
            continue;
        auto& last = last_[static_cast<std::size_t>(t)][static_cast<std::size_t>(slot)];
        if (last.step == s.step && last.value == value)
            continue;
        last = {s.step, value};
        out.push_back({s.scene, s.pattern, t, s.step, static_cast<core::model::LockSlot>(slot), value, &spec});
    }
}

void StripRecorder::frame(const StripRecordState& s, const core::model::RibbonSettings& ribbon,
                          std::vector<LockWrite>& out)
{
    if (! s.recording || ! s.contact || s.step < 0 || s.step >= core::model::kSteps || s.tracks == 0)
    {
        if (touching_)
            reset(); // the next touch writes its first value even on the same step
        return;
    }
    touching_ = true;
    const auto mode = static_cast<core::ribbon::Mode>(s.mode);
    if (mode == core::ribbon::Mode::Pitch)
    {
        if (pitch_ != nullptr) // RIB-002 range in cents → playback.pitch semitones
            emit(s, *pitch_,
                 core::ribbon::pitchCents(s.position, ribbon.pitchBottomCents, ribbon.pitchTopCents) / 100.0f, out);
        return;
    }
    if (mode != core::ribbon::Mode::User1 && mode != core::ribbon::Mode::User2)
        return; // REPEAT: not a parameter move
    const auto& items = ribbon.user[mode == core::ribbon::Mode::User1 ? 0 : 1];
    for (const auto& item : items)
    {
        if (item.target.empty())
            continue;
        const auto* spec = registry_.find(item.target);
        if (spec == nullptr)
            continue;
        const float v01 = core::ribbon::userValue(s.position, static_cast<float>(item.bottom), static_cast<float>(item.top));
        emit(s, *spec, spec->minValue + v01 * (spec->maxValue - spec->minValue), out);
    }
}
} // namespace ui::input::ext
