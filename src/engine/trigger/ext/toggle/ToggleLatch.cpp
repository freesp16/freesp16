#include "engine/trigger/ext/toggle/ToggleLatch.h"

namespace engine::trigger::ext
{
void ToggleLatch::bindParams(const std::function<core::ParamId(std::string_view)>& resolve) // rt-ok: set-up
{
    if (! resolve)
        return;
    toggleId_ = resolve(kParamId);
    triggerModeId_ = resolve("track.triggerMode");
}

bool ToggleLatch::onParam(const ParamChange& c, TriggerRouter& router) noexcept
{
    if (c.scope > 0xFF)
        return false;
    const int s = (c.scope >> 4) & 15, t = c.scope & 15;
    const auto bit = static_cast<std::uint16_t>(1u << t);
    if (toggleId_.valid() && c.param == toggleId_)
    {
        const bool on = c.value >= 0.5f, was = enabled(s, t);
        if (on == was)
            return true;
        toggle_[static_cast<std::size_t>(s)] =
            static_cast<std::uint16_t>(on ? toggle_[static_cast<std::size_t>(s)] | bit : toggle_[static_cast<std::size_t>(s)] & ~bit);
        anyToggle_ = false;
        for (const auto m : toggle_)
            anyToggle_ = anyToggle_ || m != 0;
        router.setTrackParam(s, t, TrackField::TriggerMode,
                             on ? 1.0f : static_cast<float>(mode_[static_cast<std::size_t>(s)][static_cast<std::size_t>(t)]));
        if (! on && s == scene_ && latched(t))
            drop(t);
        return true;
    }
    if (triggerModeId_.valid() && c.param == triggerModeId_)
    {
        mode_[static_cast<std::size_t>(s)][static_cast<std::size_t>(t)] = c.value >= 0.5f ? 1 : 0;
        return enabled(s, t); // a Toggle track stays GATE in the router; the model's value is kept for TOGGLE off
    }
    return false;
}

int ToggleLatch::trackOf(const TriggerNow& hit, const TriggerRouter& r) const noexcept
{
    if (hit.pad < 0 || hit.pad >= kTracks)
        return hit.track & 15;
    const auto& pads = r.padState();
    if (pads.shift && pads.mode != PadMode::Mute)
        return -1; // SHIFT + pad selects (PAD-004)
    switch (pads.mode)
    {
        case PadMode::Play: return hit.track & 15;
        case PadMode::Track:
        case PadMode::Mute: return -1;
        case PadMode::Slice:
        case PadMode::Scale: return pads.selected & 15;
    }
    return -1;
}

void ToggleLatch::drop(int track) noexcept
{
    const auto bit = static_cast<std::uint16_t>(1u << (track & 15));
    latched_ = static_cast<std::uint16_t>(latched_ & ~bit);
    pendingRelease_ = static_cast<std::uint16_t>(pendingRelease_ | bit);
}

bool ToggleLatch::filter(TriggerNow& hit, const TriggerRouter& r, std::uint64_t block) noexcept
{
    if (! anyToggle_ && latched_ == 0 && swallowPad_ == 0 && swallowTrack_ == 0)
        return true; // no Toggle track anywhere: the hit is untouched (the flag-off path)
    if (r.scene() != scene_)
    {
        for (int t = 0; t < kTracks; ++t)
            if (latched(t))
                drop(t);
        scene_ = r.scene();
    }
    const bool press = hit.velocity > 0;
    const bool padHit = hit.pad >= 0 && hit.pad < kTracks;
    if (! press)
    {
        auto& swallow = padHit ? swallowPad_ : swallowTrack_;
        const auto bit = static_cast<std::uint16_t>(1u << (padHit ? hit.pad : hit.track & 15));
        if ((swallow & bit) == 0)
            return true;
        swallow = static_cast<std::uint16_t>(swallow & ~bit);
        return false; // the release of a toggling press
    }
    const int t = trackOf(hit, r);
    if (t < 0 || ! enabled(scene_, t))
        return true;
    const auto tbit = static_cast<std::uint16_t>(1u << t);
    if (padHit)
        swallowPad_ = static_cast<std::uint16_t>(swallowPad_ | (1u << hit.pad));
    else
        swallowTrack_ = static_cast<std::uint16_t>(swallowTrack_ | tbit);
    if (latched(t))
    {
        // The second press stops the note: the release of whatever latched it.
        latched_ = static_cast<std::uint16_t>(latched_ & ~tbit);
        const int lp = latchPad_[static_cast<std::size_t>(t)];
        hit = lp >= 0 ? TriggerNow {static_cast<std::uint8_t>(lp), 0, hit.note, static_cast<std::int8_t>(lp)}
                      : TriggerNow {static_cast<std::uint8_t>(t), 0, latchNote_[static_cast<std::size_t>(t)], -1};
        return true;
    }
    latched_ = static_cast<std::uint16_t>(latched_ | tbit);
    latchPad_[static_cast<std::size_t>(t)] = static_cast<std::int8_t>(padHit ? hit.pad : -1);
    latchNote_[static_cast<std::size_t>(t)] = hit.note;
    latchBlock_[static_cast<std::size_t>(t)] = block;
    return true;
}

void ToggleLatch::flush(TriggerRouter& router) noexcept
{
    if (pendingRelease_ == 0 && (latched_ == 0 || router.scene() == scene_))
        return;
    if (router.scene() != scene_)
    {
        for (int t = 0; t < kTracks; ++t)
            if (latched(t))
                drop(t);
        scene_ = router.scene();
    }
    for (int t = 0; t < kTracks; ++t)
    {
        const auto bit = static_cast<std::uint16_t>(1u << t);
        if ((pendingRelease_ & bit) == 0)
            continue;
        const int lp = latchPad_[static_cast<std::size_t>(t)];
        const TriggerNow rel = lp >= 0 ? TriggerNow {static_cast<std::uint8_t>(lp), 0, 60, static_cast<std::int8_t>(lp)}
                                       : TriggerNow {static_cast<std::uint8_t>(t), 0, latchNote_[static_cast<std::size_t>(t)], -1};
        if (! router.pushLive(rel))
            return; // the block is full: the rest next block
        pendingRelease_ = static_cast<std::uint16_t>(pendingRelease_ & ~bit);
    }
}

void ToggleLatch::endBlock(std::span<const TrackSource, kTracks> sources, std::uint64_t block) noexcept
{
    if (latched_ == 0)
        return;
    for (int t = 0; t < kTracks; ++t)
        if (latched(t) && latchBlock_[static_cast<std::size_t>(t)] != block &&
            ! sources[static_cast<std::size_t>(t)].sounding())
            latched_ = static_cast<std::uint16_t>(latched_ & ~(1u << t)); // sample end, choke, stop
}
} // namespace engine::trigger::ext
