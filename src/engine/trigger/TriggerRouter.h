// Step 6 of the engine block (audio-engine.md "Per-block processing order"): every
// trigger that reaches a track this block — live pad / MIDI hits from the TriggerSink and posted TriggerNow (UI-118,
// at frame 0: one block of latency, threading.md), sequencer steps (through SequencerStage), retrigger and
// repeat hits — merged, stably sorted by frame and applied in frame order:
//
//   pad routing   a pad hit (TriggerNow::pad ≥ 0) plays by the pad mode (PAD-001): PLAY = track n, TRACK / MUTE =
//                 no sound (the message thread selects / mutes), SLICE = the selected track playing the pad's slice
//                 (a slice-number lock, engine/voice/SliceMapPads.h), SCALE = the selected track at the pad's scale
//                 degree (PAD-010…013); SHIFT held = select only (PAD-004). A pad release goes to the track its press
//                 played, whatever the mode is by then.
//   MIDI          TriggerNow::pad < 0: track = TriggerNow::track, semitones = note − 60, velocity 0 = note off (TRK-011)
//   choke         a Full trigger stops every other track of its choke group in the current scene with the voice's
//                 2 ms fade (TRK-020); groups are one 16-bit mask per scene and group
//   gate          GATE tracks take pad/MIDI hits gated, release on pad up; sequencer events are One Shot (SEQ-015)
//   locks         the mixer's volume/pan/send/mute lock slots per trigger (Mixer::applyLocks, null = revert)
//   ducks         Full/Half from any source on an unmuted track (SEQ-027, consumed by the Ducker)
//   live hits     Full pad/MIDI hits also go to the LiveHitSink (the sequencer records them, PAD-005), and the touch
//                 strip's REPEAT hits (recorded onto the steps while REC is armed; the reference does not, RIB-008)
//
// blockEvents() lists the block's applied events (with `ducks` set) for the Ducker; hits(t) counts track t's
// Full / Half triggers for the pad-flash feed (PlayheadFeed::trackHits).
// Performance: ≤ 6 µs per block on the A9 for 64 events — insertion sort, no allocation, no
// virtual call per event except the optional LiveHitSink (one per live hit).
#pragma once

#include "engine/EngineApi.h"
#include "engine/mixer/Mixer.h"
#include "engine/trigger/TrackParams.h"
#include "engine/trigger/TrackSource.h"
#include "engine/trigger/TriggerEvent.h"
#include "engine/voice/SliceMapPads.h"

#include <array>
#include <cstdint>
#include <span>

namespace engine::trigger
{
class LiveHitSink
{
public:
    virtual ~LiveHitSink() = default;
    virtual void onLiveHit(const TriggerEvent&) noexcept = 0; // the sequencer records (PAD-005)
};

class TriggerRouter
{
public:
    static constexpr int kMaxEvents = 64;     // per block; further events of a block are refused
    static constexpr int kMaxLiveEvents = 48; // live hits per block; the other 16 are kept for sequencer / repeat
    static constexpr int kScenes = 16;

    TriggerRouter() noexcept; // out of line: keeps the 16 × 16 settings' initialisation out of the engine's TU (GCC 11)
    TriggerRouter(const TriggerRouter&) = delete;
    TriggerRouter& operator= (const TriggerRouter&) = delete;

    // ---- message thread ----
    void prepare(double sampleRate, int maxBlock);
    // The 16 track sources it drives and the mixer for the lock slots / mute state (either may be null in tests).
    void attach(std::span<TrackSource, kTracks> sources, mixer::Mixer* mixer) noexcept;
    void setLiveHitSink(LiveHitSink* sink) noexcept { liveHits_ = sink; }
    // SLICE pad mode hits carry their slice as a slice-number lock from `pads` (SliceMapPads); null =
    // SLICE mode plays the selected track's whole region.
    void setSlicePads(voice::SlicePadLocks* pads) noexcept { slicePads_ = pads; }

    // ---- audio thread ----
    bool push(const TriggerEvent&) noexcept;    // sequencer, MIDI, repeat; false = this block is full
    // Pad / MIDI hit from the bridge or the TriggerSink, frame 0; false = this block's live share is full (the caller
    // keeps it for the next block, in order).
    bool pushLive(const TriggerNow&) noexcept;
    void setChoke(int scene, int track, std::uint8_t group) noexcept; // 0 = none, 1–8 (TRK-020)
    // ParamChange targets (scope = (scene << 4) | track; pads: any scope). Mode changes reach the track source of the
    // current scene at once (TRK-002).
    void setTrackParam(int scene, int track, TrackField, float value) noexcept;
    void setPadParam(PadField, float value) noexcept;
    void setScene(int scene) noexcept;
    void process(BlockContext&) noexcept; // sort by frame, choke, drive the 16 TrackSources

    std::span<const TriggerEvent> blockEvents() const noexcept
    {
        return {applied_.data(), static_cast<std::size_t>(numApplied_)};
    }
    const TrackSettings& settings(int scene, int track) const noexcept { return settings_[scene & 15][track & 15]; }
    const PadState& padState() const noexcept { return pads_; }
    int scene() const noexcept { return scene_; }
    std::uint16_t chokeMask(int scene, int group) const noexcept
    {
        return group >= 1 && group <= 8 ? groups_[scene & 15][group - 1] : 0;
    }
    int pending() const noexcept { return numEvents_; } // events pushed for this block so far
    // Those events, in push order (the perf stage reads the sequencer's steps before process(): ratchets, chokes).
    std::span<const TriggerEvent> pendingEvents() const noexcept
    {
        return {events_.data(), static_cast<std::size_t>(numEvents_)};
    }
    std::uint64_t dropped() const noexcept { return dropped_; } // events refused because a block was full
    // Track t's applied Full / Half triggers so far, wrapping (PlayheadFeed::trackHits).
    std::uint16_t hits(int track) const noexcept { return hits_[static_cast<std::size_t>(track & 15)]; }

private:
    TrackSource* sources_ = nullptr; // kTracks of them
    mixer::Mixer* mixer_ = nullptr;
    LiveHitSink* liveHits_ = nullptr;
    voice::SlicePadLocks* slicePads_ = nullptr;
    int scene_ = 0;
    TrackSettings settings_[kScenes][kTracks]; // default-initialised (no braces: GCC 11 build time)
    std::array<std::array<std::uint16_t, 8>, kScenes> groups_ {}; // bit t = track t is in group g+1
    PadState pads_ {};
    std::array<std::int8_t, kTracks> heldTrack_ {}; // per pad: the track its press played (−1 none)
    std::array<std::int8_t, kTracks> heldSemis_ {};
    std::array<TriggerEvent, kMaxEvents> events_, applied_; // no braces (GCC 11 build time): written before read
    int numEvents_ = 0, numApplied_ = 0;
    std::uint64_t dropped_ = 0;
    std::array<std::uint16_t, kTracks> hits_ {};
    std::array<std::int8_t, kTracks> chokedAt_ {}, startedAt_ {}; // this block's choke / hit frame per track (−1 none)
    std::uint32_t mixerLocked_ = 0; // bit t = the last trigger of track t locked a mixer slot (its revert is due)
    static constexpr std::uint64_t kMixerLockSlots = 1u << mixer::kLockVolume | 1u << mixer::kLockPan
                                                     | 1u << mixer::kLockSend | 1u << mixer::kLockMute;
};
} // namespace engine::trigger
