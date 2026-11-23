#include "ui/input/ext/striprecord/StripRecorder.h"

#include "core/commands/sequencer/SequencerCommands.h"
#include "core/util/ribbon/Ribbon.h"

#include <algorithm>
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

void StripHeld::onPad(int pad, bool pressed, int padMode, bool shift, int selected) noexcept
{
    if (pad < 0 || pad > 15)
        return;
    auto& t = padTrack_[static_cast<std::size_t>(pad)];
    if (! pressed)
    {
        t = -1; // a release goes with the press, whatever the mode is by then
        return;
    }
    // The track the press plays (the router's pad routing): PLAY = pad n, SLICE / SCALE = the selected track;
    // TRACK / MUTE pads and SHIFT + pad make no sound.
    t = shift ? -1 : padMode == 0 ? static_cast<std::int8_t>(pad)
                   : (padMode == 3 || padMode == 4) ? static_cast<std::int8_t>(selected & 15) : std::int8_t {-1};
}

std::uint16_t StripHeld::tracks(int applyTo, int selected, bool holding) noexcept
{
    if (applyTo == 1)
        return static_cast<std::uint16_t>(1u << (selected & 15));
    if (applyTo == 2)
        return 0xFFFF;
    std::uint16_t held = 0;
    for (const auto t : padTrack_)
        if (t >= 0)
            held = static_cast<std::uint16_t>(held | (1u << t));
    sticky_ = holding ? static_cast<std::uint16_t>(sticky_ | held) : 0; // HOLD: they keep the value (RIB-005)
    return static_cast<std::uint16_t>(held | sticky_);
}

void StripRecorder::reset() noexcept
{
    for (auto& t : last_)
        t.fill({});
    touching_ = false;
}

void StripRecorder::emit(const StripRecordState& s, const core::ParamSpec& spec, float value, bool addPitch,
                         std::vector<LockWrite>& out)
{
    if (! spec.lockable || spec.scope != core::ParamScope::Track)
        return; // FX-094: Send / Master FX and preferences are no step-lock targets
    const int slot = core::commands::sequencer::lockSlotOf(spec, registry_);
    if (slot < 0 || slot >= core::model::kLockSlots)
        return;
    const int length = std::clamp(s.length, 1, core::model::kSteps);
    for (int t = 0; t < core::model::kTracks; ++t)
    {
        if ((s.tracks >> t & s.record >> t & 1u) == 0)
            continue;
        float v = addPitch ? s.pitch[static_cast<std::size_t>(t)] + value : value;
        if (integral(spec))
            v = std::nearbyint(v);
        v = spec.clamp(v);
        auto& last = last_[static_cast<std::size_t>(t)][static_cast<std::size_t>(slot)];
        if (last.step == s.step && last.value == v)
            continue;
        // The steps the playhead passed since this track's previous write of the touch (a slow frame): the value too.
        if (last.step >= 0 && last.step != s.step)
        {
            const int gap = ((s.step - last.step) % length + length) % length;
            if (gap > 1 && gap <= 16)
                for (int k = 1; k < gap; ++k)
                    out.push_back({s.scene, s.pattern, t, (last.step + k) % length,
                                   static_cast<core::model::LockSlot>(slot), v, &spec});
        }
        last = {s.step, v};
        out.push_back({s.scene, s.pattern, t, s.step, static_cast<core::model::LockSlot>(slot), v, &spec});
    }
}

void StripRecorder::frame(const StripRecordState& s, const core::model::RibbonSettings& ribbon,
                          std::vector<LockWrite>& out)
{
    if (! s.recording || ! s.contact || s.step < 0 || s.step >= core::model::kSteps || (s.tracks & s.record) == 0)
    {
        if (touching_)
            reset(); // the next touch writes its first value even on the same step
        return;
    }
    touching_ = true;
    const auto mode = static_cast<core::ribbon::Mode>(s.mode);
    if (mode == core::ribbon::Mode::Pitch)
    {
        if (pitch_ != nullptr) // RIB-002 range in cents → semitones on the track's own pitch
            emit(s, *pitch_,
                 core::ribbon::pitchCents(s.position, ribbon.pitchBottomCents, ribbon.pitchTopCents) / 100.0f, true,
                 out);
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
        emit(s, *spec, spec->minValue + v01 * (spec->maxValue - spec->minValue), false, out);
    }
}
} // namespace ui::input::ext
