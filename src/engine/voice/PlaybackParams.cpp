#include "engine/voice/PlaybackParams.h"

#include <cmath>

namespace engine::voice
{
namespace
{
std::int64_t toFrames(float v) noexcept
{
    return v <= 0.0f ? 0 : static_cast<std::int64_t>(std::llround(static_cast<double>(v)));
}

PlaybackField fieldOfSlot(int slot) noexcept
{
    switch (slot)
    {
        case lockslot::Start: return PlaybackField::Start;
        case lockslot::LoopStart: return PlaybackField::LoopStart;
        case lockslot::Length: return PlaybackField::Length;
        case lockslot::Pitch: return PlaybackField::Pitch;
        case lockslot::TimeStretch: return PlaybackField::TimeStretch;
        case lockslot::Loop: return PlaybackField::Loop;
        case lockslot::Reverse: return PlaybackField::Reverse;
        default: return PlaybackField::Count;
    }
}
} // namespace

PlaybackField playbackFieldOf(std::string_view id) noexcept
{
    if (id == "playback.pitch")
        return PlaybackField::Pitch;
    if (id == "playback.start")
        return PlaybackField::Start;
    if (id == "playback.loopStart")
        return PlaybackField::LoopStart;
    if (id == "playback.length")
        return PlaybackField::Length;
    if (id == "playback.loop")
        return PlaybackField::Loop;
    if (id == "playback.timeStretch")
        return PlaybackField::TimeStretch;
    if (id == "playback.reverse")
        return PlaybackField::Reverse;
    if (id == "playback.bypass")
        return PlaybackField::Bypass;
    return PlaybackField::Count;
}

void setField(PlaybackParams& p, PlaybackField field, float value) noexcept
{
    switch (field)
    {
        case PlaybackField::Pitch:
        {
            const float c = value * kPitchCentsPerUnit;
            p.pitchCents = c < -kMaxPitchCents ? -kMaxPitchCents : (c > kMaxPitchCents ? kMaxPitchCents : c);
            break;
        }
        case PlaybackField::Start: p.region.start = toFrames(value); break;
        case PlaybackField::LoopStart: p.region.loopStart = toFrames(value); break;
        case PlaybackField::Length: p.region.length = toFrames(value); break;
        case PlaybackField::Loop: p.region.loop = value >= 0.5f; break;
        case PlaybackField::Reverse: p.region.reverse = value >= 0.5f; break;
        case PlaybackField::Bypass: p.bypass = value >= 0.5f; break;
        case PlaybackField::TimeStretch:
        {
            const long m = std::lround(value);
            p.stretch = m <= 0 ? StretchMode::Off : (m == 1 ? StretchMode::Resample : StretchMode::MasterTempo);
            break;
        }
        case PlaybackField::Count: break;
    }
}

bool PlaybackParamBlock::set(PlaybackField field, std::uint16_t scope, float value) noexcept
{
    if (field == PlaybackField::Count || scope > 0xFF)
        return false;
    setField(params_[(scope >> 4) & 15][scope & 15], field, value);
    return true;
}

PlaybackParams PlaybackLocks::resolve(const PlaybackParams& base, std::uint64_t mask, const float* values,
                                      const std::uint8_t* change) noexcept
{
    PlaybackParams p = base;
    if (values == nullptr)
        mask = 0;
    for (const int slot : lockslot::kPlaybackSlots)
    {
        const std::uint64_t bit = std::uint64_t {1} << slot;
        if (mask & bit)
        {
            const float v = values[slot];
            setField(p, fieldOfSlot(slot), v);
            const auto kind = change != nullptr ? static_cast<LockChange>(change[slot]) : LockChange::Revert;
            if (kind == LockChange::Revert)
                held_ &= ~bit;
            else
            {
                held_ |= bit;
                heldValue_[slot] = v;
            }
        }
        else if (held_ & bit)
            setField(p, fieldOfSlot(slot), heldValue_[slot]);
    }
    return p;
}
} // namespace engine::voice
