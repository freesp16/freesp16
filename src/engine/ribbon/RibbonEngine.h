// The touch strip in the engine (spec 02-features/touch-strip.md RIB-001…007/010, effects/parameters.md FX-095):
// the four modes PITCH → REPEAT → USER1 → USER2, HOLD, the pad rule and the
// LED mask, on the audio thread, inside the engine block (engine::Engine):
//
//   step 1   onParam(ribbon.*)          mode / hold from the panel, the project's ribbon settings (catalogue/ribbon.cpp)
//            onLive(TriggerNow)          the strip stream: position + contact, pad pressure (core/util/ribbon/Ribbon.h
//                                        pad codes; the device's reader thread pushes them into the TriggerSink)
//   step 6   collect(ctx, router)        REPEAT hits for the held pads → the trigger router, before it sorts the block
//            observe(events, pads)       after the router: the held pads (Full / Release from pads), the selected track
//            publish(matrix)             before ModMatrix::applyBlock: PITCH = ModSource::Ribbon Offset on
//                                        playback.pitch, USER items = Override on their targets, of the target tracks;
//                                        incoming pitch bend (PITCH mode only, RIB-007) = ModSource::PitchBend Offset
//
// Laws (core::ribbon): PITCH centre 64 = 0, linear to bottomCent / topCent (−200 / +200, RIB-002); REPEAT zones 1/4 …
// 1/32 from the bottom (RIB-003, our choice); USER value = bottom + (top − bottom) · position / 127 (RIB-004).
//
// Targets (our choices where the spec is silent):
//   - The strip acts while a pad is held (RIB-010); touching first and pressing a pad later applies too. A held pad =
//     a pad press the router routed to a track (PLAY: its track, SLICE / SCALE: the selected track); TRACK / MUTE
//     pads make no sound and are not held. With the preference ribbon.applyTo = SELECTED / ALL, PITCH and USER act on
//     the selected track / every track without a held pad.
//   - Release (no HOLD) springs back: the ribbon's contributions are cleared, the destinations return to their base
//     (stored or locked) value. A pad released while the strip is touched releases its track the same way.
//   - HOLD (RIB-005): the last value stays after the strip is released; the tracks that received it under HOLD keep
//     it even when their pads are released, and pads pressed later receive it too. HOLD off releases everything; a
//     MODE change cancels HOLD (and the UI turns the HOLD lamp off).
//   - REPEAT repeats the held pads only (RIB-003 "while a pad is held"), also under HOLD (the latched rate). Running:
//     hits on the transport grid (k · interval in PPQ, the sequencer's frame rounding); stopped: free-running from the
//     moment REPEAT became active (first hit one interval later; the pad hit itself is the first note). A rate change
//     applies from the last hit. A pad newly held skips repeats within half an interval of its own hit (no flam).
//     Velocity = the pad's last pressure (pressure 0 → 1), the hit's velocity until a pressure arrives (RIB-003).
//     A repeat replays the last pad hit of its track: its scale note (SCALE pad mode: the hit's semitones) and, in
//     the track source, its slice (SLICE pad mode). With REC armed the sequencer records the repeats (RIB-008, ours).
//
// Real-time: fixed arrays, no allocation, no locks, no virtual call; publish() touches the matrix only when the
// ribbon's output changed. Budget: ≤ 1.5k cycles per block with 8 USER items × 4 held tracks (bench_ribbon).
#pragma once

#include "core/util/ribbon/Ribbon.h"
#include "engine/EngineApi.h"
#include "engine/modulation/ModMatrix.h"
#include "engine/trigger/TrackParams.h"
#include "engine/trigger/TriggerEvent.h"

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

namespace engine::ribbon
{
using Mode = core::ribbon::Mode;
using ApplyTo = core::ribbon::ApplyTo;

struct UserItem
{
    core::ParamId target {}; // invalid = empty slot
    float bottom01 = 0.0f, top01 = 1.0f;
};

class RibbonEngine
{
public:
    static constexpr int kMaxHits = kTracks; // REPEAT: ≤ 1 hit per track per block (1/32 at 300 BPM = 1102 frames)
    static constexpr int kMaxOut = 1 + core::ribbon::kUserItems; // per track: the pitch offset or 8 USER items

    // ---- message thread (engine set-up) ----
    // Resolves ribbon.* and playback.pitch (an unknown id stays invalid: that message is never consumed).
    void bindParams(const std::function<core::ParamId(std::string_view)>& resolve); // rt-ok: set-up only
    core::ParamId paramId(int i) const noexcept { return ids_[static_cast<std::size_t>(i)]; } // ParamIndex order
    enum ParamIndex : int { kMode, kHold, kPitchTop, kPitchBottom, kUserTarget, kUserTop, kUserBottom, kApplyTo,
                            kParams };
    void prepare(double sampleRate) noexcept;

    // ---- audio thread ----
    bool onParam(const ParamChange&) noexcept; // a ribbon.* message (consumed) or false
    // The strip / pressure stream (TriggerNow with a core::ribbon pad code); false = an ordinary trigger. `pads` maps
    // a pad's pressure to its track like the router does.
    bool onLive(const TriggerNow&, const trigger::PadState& pads, int heldTrack = -1) noexcept;
    void onStrip(int position, bool contact) noexcept;
    void onPressure(int track, int pressure) noexcept;
    void onPitchBend(int track, int value14) noexcept; // RIB-007, MID-032; 8192 = centre
    void setMode(Mode) noexcept;                        // a change cancels HOLD (RIB-005)
    void setHold(bool) noexcept;
    void setApplyTo(ApplyTo) noexcept;
    void setPitchRange(int bottomCents, int topCents) noexcept;
    void setItem(int set, int item, const UserItem&) noexcept;

    // Step 6, before the router: this block's REPEAT hits (frame-accurate), at most kMaxHits; returns the count.
    void stopRepeats() noexcept;
    void swapTracks(int a, int b) noexcept;
    int collectHits(const BlockContext&, std::span<trigger::TriggerEvent, kMaxHits> out) noexcept;
    template <class Sink> // TriggerRouter (push(const TriggerEvent&) → bool) or a test sink
    void collect(const BlockContext& ctx, Sink& sink) noexcept
    {
        std::array<trigger::TriggerEvent, kMaxHits> hits;
        const int n = collectHits(ctx, hits);
        for (int i = 0; i < n; ++i)
            if (! sink.push(hits[static_cast<std::size_t>(i)]))
                ++dropped_;
    }
    // Step 6, after the router: the block's applied events (held pads) and the pad state (selected track).
    void observe(std::span<const trigger::TriggerEvent> events, const trigger::PadState& pads) noexcept;
    // Before ModMatrix::applyBlock: the changes of the ribbon's contributions (set / clear of the Ribbon and PitchBend
    // sources). Matrix = modulation::ModMatrix (or a test double with the same set / clear signatures).
    template <class Matrix>
    void publish(Matrix& matrix) noexcept;

    // ---- inspection (audio thread; tests, benches, the LED mask for a LampSink) ----
    Mode mode() const noexcept { return mode_; }
    bool hold() const noexcept { return hold_; }
    bool contact() const noexcept { return contact_; }
    int position() const noexcept { return position_; }
    int effectivePosition() const noexcept; // touched, or latched under HOLD; −1 = none
    std::uint16_t heldTracks() const noexcept { return held_; }
    std::uint16_t targetTracks() const noexcept { return targets_; } // PITCH / USER act on these
    std::uint8_t ledMask() const noexcept;                            // RIB-006 (core::ribbon::ledMask)
    bool repeating() const noexcept { return repeatActive_; }
    double repeatIntervalFrames(double bpm) const noexcept; // the current zone's interval (0 = not REPEAT)
    float pitchCents() const noexcept;                       // the PITCH law at the effective position
    const UserItem& item(int set, int item) const noexcept { return items_[set & 1][item & 7]; }
    std::uint64_t droppedHits() const noexcept { return dropped_; }
    std::uint64_t matrixRefusals() const noexcept { return refused_; } // set() refused (no free destination slot)

private:
    struct Out
    {
        core::ParamId dest {};
        float value = 0.0f;
        bool override = false;
    };
    // The wanted contributions of every target track (≤ kMaxOut), from mode / position / items.
    int wanted(std::array<Out, kMaxOut>& out) const noexcept;
    void changed() noexcept { dirty_ = true; }
    void releaseAll() noexcept; // HOLD off / mode change: nothing latched, nothing sticky

    std::array<core::ParamId, kParams> ids_ {};
    core::ParamId pitchId_ {};
    double sampleRate_ = 44100.0;

    Mode mode_ = Mode::Pitch;
    ApplyTo applyTo_ = ApplyTo::HeldPads;
    bool hold_ = false, contact_ = false, latched_ = false;
    int position_ = core::ribbon::kCentre, latchedPosition_ = core::ribbon::kCentre;
    int bottomCents_ = -200, topCents_ = 200;
    UserItem items_[core::ribbon::kUserSets][core::ribbon::kUserItems] {};

    // Pads (observe): presses minus releases per track, the hit velocity, the last pressure (−1 = none yet).
    std::array<std::uint8_t, kTracks> heldCount_ {};
    std::array<std::uint8_t, kTracks> velocity_ {};
    std::array<std::int8_t, kTracks> semitones_ {}; // the held pad's note (SCALE pad mode): the repeats play it too
    std::array<std::int16_t, kTracks> pressure_ {};
    std::array<double, kTracks> sincePress_ {}; // frames since the pad's own hit (REPEAT no-flam rule)
    std::uint16_t held_ = 0, sticky_ = 0, targets_ = 0;
    std::uint8_t selected_ = 0;

    // REPEAT.
    std::uint16_t stoppedRepeats_ = 0; // held pads silenced by STOP, until a fresh pad or strip press
    bool repeatActive_ = false;
    double phase_ = 0.0; // frames since the last repeat hit (or since REPEAT became active)
    std::uint64_t dropped_ = 0;

    // Published contributions (publish's diff).
    std::array<std::array<Out, kMaxOut>, kTracks> pub_ {};
    std::array<std::uint8_t, kTracks> pubCount_ {};
    std::uint16_t pubMask_ = 0, retry_ = 0;
    std::array<float, kTracks> bend_ {}, bendPub_ {}; // pitch bend offsets (normalised) wanted / published
    std::uint16_t bendMask_ = 0, bendPubMask_ = 0;
    bool dirty_ = true, bendDirty_ = false;
    std::uint64_t refused_ = 0;
};

template <class Matrix>
void RibbonEngine::publish(Matrix& matrix) noexcept
{
    using modulation::ModKind;
    using modulation::ModSource;
    if (dirty_ || retry_ != 0)
    {
        std::uint16_t retry = 0;
        std::array<Out, kMaxOut> want; // the same for every target track
        const int wantN = targets_ != 0 ? wanted(want) : 0;
        for (int t = 0; t < kTracks; ++t)
        {
            const auto bit = static_cast<std::uint16_t>(1u << t);
            const bool inTargets = (targets_ & bit) != 0, again = (retry_ & bit) != 0;
            if (! inTargets && ! (pubMask_ & bit))
                continue;
            const int n = inTargets ? wantN : 0;
            auto& pub = pub_[static_cast<std::size_t>(t)];
            const int had = pubCount_[static_cast<std::size_t>(t)];
            bool sameDests = had == n; // the usual case: the finger moved, the destinations did not
            for (int i = 0; i < n && sameDests; ++i)
                sameDests = pub[static_cast<std::size_t>(i)].dest == want[static_cast<std::size_t>(i)].dest;
            if (! sameDests)
                for (int i = 0; i < had; ++i) // released destinations
                {
                    bool kept = false;
                    for (int j = 0; j < n && ! kept; ++j)
                        kept = want[static_cast<std::size_t>(j)].dest == pub[static_cast<std::size_t>(i)].dest;
                    if (! kept)
                        matrix.clear(t, pub[static_cast<std::size_t>(i)].dest, ModSource::Ribbon);
                }
            for (int j = 0; j < n; ++j)
            {
                const Out& w = want[static_cast<std::size_t>(j)];
                bool same = false;
                if (sameDests)
                    same = pub[static_cast<std::size_t>(j)].value == w.value && pub[static_cast<std::size_t>(j)].override == w.override;
                else
                    for (int i = 0; i < had && ! same; ++i)
                        same = pub[static_cast<std::size_t>(i)].dest == w.dest && pub[static_cast<std::size_t>(i)].value == w.value
                               && pub[static_cast<std::size_t>(i)].override == w.override;
                if ((! same || again)
                    && ! matrix.set(t, w.dest, ModSource::Ribbon, w.override ? ModKind::Override : ModKind::Offset, w.value))
                {
                    ++refused_;
                    retry = static_cast<std::uint16_t>(retry | bit);
                }
                pub[static_cast<std::size_t>(j)] = w;
            }
            pubCount_[static_cast<std::size_t>(t)] = static_cast<std::uint8_t>(n);
            pubMask_ = static_cast<std::uint16_t>(n > 0 ? (pubMask_ | bit) : (pubMask_ & ~bit));
        }
        retry_ = retry;
        dirty_ = false;
    }
    if (bendDirty_)
    {
        bendDirty_ = false;
        for (int t = 0; t < kTracks; ++t)
        {
            const auto bit = static_cast<std::uint16_t>(1u << t);
            const bool want = (bendMask_ & bit) != 0, had = (bendPubMask_ & bit) != 0;
            if (want && (! had || bendPub_[static_cast<std::size_t>(t)] != bend_[static_cast<std::size_t>(t)]))
            {
                if (matrix.set(t, pitchId_, ModSource::PitchBend, ModKind::Offset, bend_[static_cast<std::size_t>(t)]))
                {
                    bendPub_[static_cast<std::size_t>(t)] = bend_[static_cast<std::size_t>(t)];
                    bendPubMask_ = static_cast<std::uint16_t>(bendPubMask_ | bit);
                }
                else
                {
                    ++refused_;
                    bendDirty_ = true; // retried next block
                }
            }
            else if (! want && had)
            {
                matrix.clear(t, pitchId_, ModSource::PitchBend);
                bendPubMask_ = static_cast<std::uint16_t>(bendPubMask_ & ~bit);
            }
        }
    }
}
} // namespace engine::ribbon
