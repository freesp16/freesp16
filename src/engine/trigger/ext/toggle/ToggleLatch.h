// The `Toggle` trigger mode (TRK-012, P3, our extension; flag `ext.trk.toggle`, off by default):
// the first pad hit starts the track's note, the next hit on that track stops it; releasing the pad does nothing.
//
// Shape of the extension (our design; no frozen contract changes, no edits of the trigger router):
//   - TOGGLE is a per-[scene][track] extension value, ParamChange "ext.toggle.track" (core/params/catalogue/
//     ext_trk_toggle.cpp; stored in Track::extras "native.ext.trk.toggle", a file written with it loads without the
//     flag and keeps it), not a third core::model::TriggerMode value: the frozen enum, its stored strings and the
//     TRIGGER MODE ParamSpec stay as they are. The message thread posts it only while ext.enabled +
//     ext.trk.toggle are on, so with the flag off this object never sees a message and costs one branch per hit.
//   - A Toggle track plays its latched note as a GATE note (the router's GATE path: the envelope holds until a
//     release), so "looping" is the track's own LOOP setting (TRK-014's pairing; with LOOP off the sample plays to its
//     end once and the latch clears when the voice falls silent).
//   - The latch sits in front of TriggerRouter::pushLive (Engine::addLiveTrigger): a press on an idle Toggle track
//     passes (the router starts the gated note) and its pad's release is swallowed; a press on a latched track becomes
//     the release of the pad that latched it (the router releases the track that pad played) and its own release is
//     swallowed. Pad routing follows the router's pad state (PLAY: the pad's track, SLICE / SCALE: the selected track,
//     TRACK / MUTE / SHIFT: no sound → untouched). Posted MIDI-style hits (pad < 0) latch by track the same way;
//     MIDI notes from the MIDI-in stage go straight to the router and play a Toggle track as GATE.
//   - The router's TRIGGER MODE of a Toggle track is held at GATE; the model's own value (track.triggerMode) is
//     remembered and restored when TOGGLE goes off. A latch is dropped (its release posted) when TOGGLE goes off or
//     the scene changes, and cleared when the track stops sounding (sample end, choke, stop).
//
// Real-time: audio thread only (bindParams on the message thread at set-up); fixed state, no allocation, no virtual
// call. Sequencer / control-rate work, budget < 1k cycles per block: one mask test per live hit, one pass
// over the latched tracks per block.
#pragma once

#include "engine/EngineApi.h"
#include "engine/trigger/TrackSource.h"
#include "engine/trigger/TriggerRouter.h"

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

namespace engine::trigger::ext
{
class ToggleLatch
{
public:
    static constexpr std::string_view kParamId = "ext.toggle.track";

    // ---- message thread (set-up) ----
    void bindParams(const std::function<core::ParamId(std::string_view)>& resolve); // rt-ok: set-up
    core::ParamId paramId() const noexcept { return toggleId_; }

    // ---- audio thread ----
    // ext.toggle.track (consumed), or track.triggerMode of a Toggle track (consumed: the router stays GATE); false =
    // not ours (the engine's own path applies it).
    bool onParam(const ParamChange&, TriggerRouter&) noexcept;
    // A live hit before TriggerRouter::pushLive; false = drop it. `hit` may be rewritten (a second press becomes the
    // release of the latching pad). Call only when the router can take the hit this block (pushLive would succeed).
    bool filter(TriggerNow& hit, const TriggerRouter&, std::uint64_t block) noexcept;
    // Before TriggerRouter::process: the releases of latches dropped by TOGGLE off / a scene change.
    void flush(TriggerRouter&) noexcept;
    // After the block's sources rendered: latches of tracks that fell silent clear.
    void endBlock(std::span<const TrackSource, kTracks> sources, std::uint64_t block) noexcept;

    // ---- state (tests) ----
    bool enabled(int scene, int track) const noexcept { return (toggle_[scene & 15] >> (track & 15) & 1u) != 0; }
    bool latched(int track) const noexcept { return (latched_ >> (track & 15) & 1u) != 0; }
    bool any() const noexcept { return anyToggle_; }

private:
    int trackOf(const TriggerNow& hit, const TriggerRouter& r) const noexcept; // −1 = the hit plays no track
    void drop(int track) noexcept;                                            // latch → pending release

    core::ParamId toggleId_ {}, triggerModeId_ {};
    std::array<std::uint16_t, 16> toggle_ {};                 // [scene] bit t = track t is TOGGLE
    std::array<std::array<std::uint8_t, kTracks>, 16> mode_ {}; // [scene][track] the model's TRIGGER MODE (0/1)
    std::uint16_t latched_ = 0;                               // current scene: bit t = track t's note is latched
    std::uint16_t swallowPad_ = 0, swallowTrack_ = 0;         // the next release of pad p / posted track t is dropped
    std::uint16_t pendingRelease_ = 0;                        // latched tracks whose release flush() posts
    std::array<std::int8_t, kTracks> latchPad_ {};            // per track: the pad that latched it (−1 = posted hit)
    std::array<std::uint8_t, kTracks> latchNote_ {};
    std::array<std::uint64_t, kTracks> latchBlock_ {};        // the block the latch started in
    int scene_ = 0;
    bool anyToggle_ = false;
};
} // namespace engine::trigger::ext
