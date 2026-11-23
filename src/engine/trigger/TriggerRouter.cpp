#include "engine/trigger/TriggerRouter.h"

namespace engine::trigger
{
namespace
{
std::int8_t clampSemis(int s) noexcept
{
    return static_cast<std::int8_t>(s < -127 ? -127 : (s > 127 ? 127 : s));
}
} // namespace

TriggerRouter::TriggerRouter() noexcept = default;

void TriggerRouter::prepare(double, int)
{
    heldTrack_.fill(-1);
    heldSemis_.fill(0);
    numEvents_ = numApplied_ = 0;
}

void TriggerRouter::attach(std::span<TrackSource, kTracks> sources, mixer::Mixer* mixer) noexcept
{
    sources_ = sources.data();
    mixer_ = mixer;
    heldTrack_.fill(-1);
}

bool TriggerRouter::push(const TriggerEvent& e) noexcept
{
    if (e.track >= kTracks)
        return true; // nothing to route: consumed
    if (numEvents_ == kMaxEvents)
    {
        ++dropped_;
        return false;
    }
    TriggerEvent& slot = events_[static_cast<std::size_t>(numEvents_++)];
    slot = e;
    if (slot.frame >= kBlockFrames)
        slot.frame = kBlockFrames - 1;
    return true;
}

bool TriggerRouter::pushLive(const TriggerNow& t) noexcept
{
    if (numEvents_ >= kMaxLiveEvents)
        return false; // not dropped: the engine keeps it queued for the next block
    TriggerEvent e {};
    e.frame = 0; // live triggers play at the next block's start (threading.md: one block of latency)
    e.locks = nullptr;
    e.velocity = t.velocity;
    e.kind = t.velocity > 0 ? TriggerKind::Full : TriggerKind::Release;
    if (t.pad >= 0 && t.pad < kTracks)
    {
        e.source = TriggerSource::Pad;
        const int pad = t.pad;
        if (t.velocity == 0) // the press's track hears the release, whatever the pad mode is now
        {
            if (heldTrack_[static_cast<std::size_t>(pad)] < 0)
                return true;
            e.track = static_cast<std::uint8_t>(heldTrack_[static_cast<std::size_t>(pad)]);
            e.semitones = heldSemis_[static_cast<std::size_t>(pad)];
            heldTrack_[static_cast<std::size_t>(pad)] = -1;
            return push(e);
        }
        if (pads_.shift && pads_.mode != PadMode::Mute)
            return true; // SHIFT + pad selects (PAD-004), on the message thread
        int semis = static_cast<int>(t.note) - 60;
        switch (pads_.mode)
        {
            case PadMode::Play: e.track = static_cast<std::uint8_t>(t.track & 15); break;
            case PadMode::Track:
            case PadMode::Mute: return true; // select / mute, no sound (PAD-004, PAD-006)
            case PadMode::Slice: // the pad's slice as a slice-number lock (SliceMapPads)
                e.track = pads_.selected;
                if (slicePads_ != nullptr)
                {
                    const int slice = slicePads_->slice(e.track, pad);
                    if (slice < 0)
                        return true; // the grid has no slice on this pad: silent
                    e.locks = slicePads_->lock(slice);
                }
                break;
            case PadMode::Scale:
                e.track = pads_.selected;
                semis += settings_[scene_][pads_.selected].padSemitones[static_cast<std::size_t>(pad)];
                break;
        }
        e.semitones = clampSemis(semis);
        heldTrack_[static_cast<std::size_t>(pad)] = static_cast<std::int8_t>(e.track);
        heldSemis_[static_cast<std::size_t>(pad)] = e.semitones;
        return push(e);
    }
    // MIDI note (pad −1): notes 0–15 → tracks (MIDI stage mapping), note 60 = no transpose; velocity 0 = note off.
    e.source = TriggerSource::Midi;
    e.track = static_cast<std::uint8_t>(t.track & 15);
    e.semitones = clampSemis(static_cast<int>(t.note) - 60);
    return push(e);
}

void TriggerRouter::setChoke(int scene, int track, std::uint8_t group) noexcept
{
    const int s = scene & 15, t = track & 15;
    const auto bit = static_cast<std::uint16_t>(1u << t);
    for (auto& g : groups_[static_cast<std::size_t>(s)])
        g = static_cast<std::uint16_t>(g & ~bit);
    const int gr = group > 8 ? 8 : group;
    settings_[s][t].chokeGroup = static_cast<std::uint8_t>(gr);
    if (gr > 0)
        groups_[static_cast<std::size_t>(s)][static_cast<std::size_t>(gr - 1)] |= bit;
}

void TriggerRouter::setTrackParam(int scene, int track, TrackField field, float value) noexcept
{
    const int s = scene & 15, t = track & 15;
    TrackSettings& ts = settings_[s][t];
    if (field == TrackField::ChokeGroup)
    {
        const int g = static_cast<int>(value + 0.5f);
        setChoke(s, t, static_cast<std::uint8_t>(g < 0 ? 0 : (g > 8 ? 8 : g)));
        return;
    }
    if (! applyTrackParam(ts, field, value) || s != scene_ || sources_ == nullptr)
        return;
    if (field == TrackField::Mode)
        sources_[t].setMode(ts.mode);
    else if (field == TrackField::InputSource)
        sources_[t].setInputSource(ts.input);
}

void TriggerRouter::setPadParam(PadField field, float value) noexcept { applyPadParam(pads_, field, value); }

void TriggerRouter::setScene(int scene) noexcept
{
    scene_ = scene & 15;
    if (sources_ == nullptr)
        return;
    for (int t = 0; t < kTracks; ++t)
    {
        sources_[t].setMode(settings_[scene_][t].mode);
        sources_[t].setInputSource(settings_[scene_][t].input);
    }
}

void TriggerRouter::process(BlockContext& ctx) noexcept
{
    // Stable counting sort by frame into applied_ (≤ 64 events × 64 frames: O(n + 64), no compares; live hits and
    // already ordered sequencer events cost the same as a shuffled block).
    if (numEvents_ > 1)
    {
        std::array<std::uint8_t, kBlockFrames + 1> start {};
        for (int i = 0; i < numEvents_; ++i)
            ++start[static_cast<std::size_t>(events_[static_cast<std::size_t>(i)].frame) + 1];
        for (int f = 1; f <= kBlockFrames; ++f)
            start[static_cast<std::size_t>(f)] = static_cast<std::uint8_t>(start[static_cast<std::size_t>(f)] + start[static_cast<std::size_t>(f - 1)]);
        for (int i = 0; i < numEvents_; ++i)
        {
            const TriggerEvent& e = events_[static_cast<std::size_t>(i)];
            applied_[start[e.frame]++] = e;
        }
    }
    else if (numEvents_ == 1)
        applied_[0] = events_[0];

    const auto& groups = groups_[static_cast<std::size_t>(scene_)];
    chokedAt_.fill(-1);
    startedAt_.fill(-1);
    for (int i = 0; i < numEvents_; ++i)
    {
        TriggerEvent& e = applied_[static_cast<std::size_t>(i)];
        const int t = e.track;
        const TrackSettings& ts = settings_[scene_][t];
        // MUTE MODE = STOP STEPS (Mixer::stepsMuted): a muted track's sequencer step does nothing — a lock-only event
        // without locks, so no stage (voice, choke, LFO, AFS, MIDI out, ducker) sees a hit; live hits still play.
        if (e.source == TriggerSource::Sequencer && (e.kind == TriggerKind::Full || e.kind == TriggerKind::Half)
            && mixer_ != nullptr && mixer_->stepsMuted(t))
        {
            e.kind = TriggerKind::LockOnly;
            e.locks = nullptr;
        }
        const bool hit = e.kind == TriggerKind::Full || e.kind == TriggerKind::Half;
        if (hit)
            ++hits_[static_cast<std::size_t>(t)]; // the pad-flash feed (wraps)
        if (e.kind == TriggerKind::Full && ts.chokeGroup > 0 && sources_ != nullptr) // TRK-020
            for (std::uint32_t m = groups[static_cast<std::size_t>(ts.chokeGroup - 1)] & ~(1u << t), u = 0; m != 0;
                 ++u, m >>= 1)
            {
                // Once choked in this block, a track needs no second stop until a new hit of its own (a group of 8
                // with 2 hits per track chokes each track 14 times per block).
                if (! (m & 1u) || (chokedAt_[u] >= 0 && startedAt_[u] < chokedAt_[u]))
                    continue;
                sources_[u].choke(e.frame);
                chokedAt_[u] = static_cast<std::int8_t>(e.frame);
            }
        if (e.kind == TriggerKind::Full)
            startedAt_[static_cast<std::size_t>(t)] = static_cast<std::int8_t>(e.frame);
        if (mixer_ != nullptr && (hit || e.kind == TriggerKind::LockOnly))
        {
            // Mixer::applyLocks retargets the strip and re-evaluates the mute/solo gates: only when this trigger
            // locks a mixer slot or a previous trigger of the track did (whose Revert this one is), else a no-op.
            const auto bit = 1u << t;
            const bool locks = e.locks != nullptr && e.locks->values != nullptr && (e.locks->mask & kMixerLockSlots) != 0;
            if (locks || (mixerLocked_ & bit) != 0)
            {
                mixer_->applyLocks(t, e.locks, e.frame);
                mixerLocked_ = locks ? (mixerLocked_ | bit) : (mixerLocked_ & ~bit);
            }
        }
        e.ducks = hit && ! (mixer_ != nullptr && mixer_->strip(scene_, t).mute);
        if (e.ducks) // the Ducker feed (FX-1240…1242): unmuted Full/Half hits only, the later one in frame order wins
        {
            ctx.triggerMask |= 1u << t;
            ctx.triggerOffset[static_cast<std::size_t>(t)] = static_cast<std::int16_t>(e.frame);
        }
        if (sources_ != nullptr)
            sources_[t].onEvent(e, ts.triggerMode);
        if (liveHits_ != nullptr && e.kind == TriggerKind::Full
            && (e.source == TriggerSource::Pad || e.source == TriggerSource::Midi))
            liveHits_->onLiveHit(e);
    }
    numApplied_ = numEvents_;
    numEvents_ = 0;
    if (slicePads_ != nullptr)
        slicePads_->endBlock(); // the next block's pad hits use the other lock bank
}
} // namespace engine::trigger
