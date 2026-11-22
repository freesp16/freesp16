// Ratchets (spec 02-features/sequencer.md SEQ-030, OQ-09): a sequencer
// step with RETRIGGER CNT n ≥ 2 re-fires its sound n − 1 more times, RETRIGGER SPD apart.
//
//   timing     repeat k (1 … n − 1) of a step at integer frame t0 fires at frame t0 + round(k · f · stepSamples)
//              (round half up, computed as k · stepSamples / divisor):
//              f = the speed's step fraction (1/8, 1/6, 1/4, 1/3, 1/2, 1 step: core/model/enums/ValueLists.h order),
//              stepSamples = the unswung step length at the step's tempo (15 · rate / BPM), t0 = the step's swung and
//              offset frame as the sequencer emitted it. Rounded per repeat from t0 (no accumulated drift). The
//              interval is fixed when the step fires (our choice): a tempo change meanwhile moves only later steps.
//   events     each repeat is a TriggerEvent of the step's kind (Full: envelope + sample restart; Half: envelope only,
//              OQ-09 "ratchets follow the step's kind"), source Retrigger, the step's velocity and note offset. The
//              step's locks are copied into this scheduler (the pattern object may be retired while repeats are still
//              pending, threading.md): each repeat sounds with the step's locked values, and SEQ-021's revert happens
//              at the track's next step, not at the first repeat — "locks are not re-applied" as a new step would.
//              The amp envelope answers source Retrigger by ENV-007 (AmpEnvelopeBank): it restarts with RETRIGGER on
//              or when it is idle; FULL steps always restart it.
//   pending    fixed per-track state (16 tracks × 15 pending repeats as one start frame + an index): nothing
//              allocates. A new step of the track (onStep), cancel(track) (choke, panic) and cancelAll() (STOP) drop
//              what is pending.
//   clock      a 64-bit frame counter advanced by endBlock(); emitUntil / emitBlock are relative to the current block.
//
// Real-time: every member is noexcept and allocation-free; no virtual call (the sink is a template: the TriggerRouter,
// or a test's recorder). Budget: ≤ 30 cycles per emitted event; 16 tracks × 16 repeats in one
// block ≤ 2k cycles (tests/bench/bench_perf.cpp).
#pragma once

#include "engine/EngineApi.h"
#include "engine/trigger/TriggerEvent.h"

#include <array>
#include <cstdint>

namespace engine::sequencer::perf
{
struct RetrigSpec
{
    std::uint8_t count;      // 1–16 (1 = a single trigger, no repeat)
    std::uint8_t speedIndex; // 0–5: 1/8, 1/6, 1/4, 1/3, 1/2, 1 step
};

constexpr int kRetrigSpeeds = 6;
constexpr int kMaxRetrigCount = 16;
constexpr std::uint8_t kDefaultRetrigSpeed = 2; // 1/4 step (SEQ-030, M p.40)
constexpr int kLockSlotRetrigCount = 16, kLockSlotRetrigSpeed = 17; // sequencer.md "Lockable parameters" (OQ-01)
constexpr int kLockSlots = 35;                                       // 23 track + 6 FX1 + 6 FX2

// The speed index's interval as 1/divisor step, and as a fraction (= core::model::retriggerStepFraction, checked by
// the tests). Repeat offsets are computed as k · stepSamples / divisor — one correctly rounded division of exact
// operands — so a repeat that falls exactly on half a frame (e.g. 1/6 of 2205 frames) always rounds up.
constexpr int retrigDivisor(int speedIndex) noexcept
{
    constexpr int d[kRetrigSpeeds] = {8, 6, 4, 3, 2, 1};
    return d[speedIndex < 0 ? 0 : (speedIndex >= kRetrigSpeeds ? kRetrigSpeeds - 1 : speedIndex)];
}
constexpr double retrigFraction(int speedIndex) noexcept { return 1.0 / retrigDivisor(speedIndex); }
// The unswung step length in frames at `bpm` (a step = 1/16 note = 0.25 PPQ).
constexpr double stepSamplesAt(double sampleRate, double bpm) noexcept
{
    return bpm > 0.0 ? 15.0 * sampleRate / bpm : 15.0 * sampleRate / 120.0;
}

class RetriggerScheduler
{
public:
    static constexpr int kTracks = engine::kTracks;
    static constexpr int kMaxRepeats = kMaxRetrigCount - 1;

    // Drops everything pending and restarts the clock (message thread or audio thread).
    void reset() noexcept
    {
        active_ = 0;
        blockStart_ = 0;
    }

    // A step of `track` at `offsetInBlock` (0–63) of the current block: cancels the track's pending repeats and, with
    // a count ≥ 2, schedules the new ones. The short form: a Full step without locks.
    void onStep(int track, int offsetInBlock, std::uint8_t velocity, RetrigSpec spec, double stepSamples) noexcept
    {
        trigger::TriggerEvent e {};
        e.track = static_cast<std::uint8_t>(track & 15);
        e.velocity = velocity;
        e.kind = trigger::TriggerKind::Full;
        e.source = trigger::TriggerSource::Sequencer;
        e.frame = static_cast<std::uint8_t>(offsetInBlock < 0 ? 0 : (offsetInBlock > 63 ? 63 : offsetInBlock));
        e.locks = nullptr;
        onStep(e, spec, stepSamples);
    }
    // The sequencer's step event itself (kind, velocity, note offset, frame; its locks are copied).
    void onStep(const trigger::TriggerEvent& step, RetrigSpec spec, double stepSamples) noexcept;

    void cancel(int track) noexcept { active_ = static_cast<std::uint16_t>(active_ & ~(1u << (track & 15))); }
    void cancelAll() noexcept { active_ = 0; }

    // Pushes the repeats of `track` due before `frameEnd` (0–64) of the current block into `sink` (any type with
    // `bool push(const trigger::TriggerEvent&)`, e.g. the TriggerRouter; a refused push drops that repeat). Returns
    // the number emitted.
    template <class Sink> int emitUntil(int track, int frameEnd, Sink& sink) noexcept
    {
        const int t = track & 15;
        if ((active_ >> t & 1u) == 0)
            return 0;
        Pending& p = pending_[static_cast<std::size_t>(t)];
        const std::int64_t end = blockStart_ + frameEnd;
        int n = 0;
        while (p.nextAt < end)
        {
            const std::int64_t rel = p.nextAt - blockStart_;
            trigger::TriggerEvent e {};
            e.track = static_cast<std::uint8_t>(t);
            e.velocity = p.velocity;
            e.semitones = p.semitones;
            e.kind = p.kind;
            e.source = trigger::TriggerSource::Retrigger;
            e.frame = static_cast<std::uint8_t>(rel < 0 ? 0 : rel);
            e.ducks = true;
            e.locks = p.hasLocks ? &locks_[static_cast<std::size_t>(t)].set : nullptr;
            sink.push(e);
            ++n;
            if (++p.next >= p.count)
            {
                active_ = static_cast<std::uint16_t>(active_ & ~(1u << t));
                break;
            }
            p.nextAt = p.origin + offsetOf(p, p.next);
        }
        return n;
    }
    // Every track's repeats due in this block, then endBlock(). Returns the number emitted.
    template <class Sink> int emitBlock(Sink& sink) noexcept
    {
        int n = 0;
        for (std::uint32_t m = active_; m != 0; m &= m - 1)
            n += emitUntil(__builtin_ctz(m), kBlockFrames, sink);
        endBlock();
        return n;
    }
    void endBlock() noexcept { blockStart_ += kBlockFrames; }

    // ---- state ----
    std::uint16_t pendingMask() const noexcept { return active_; }
    bool pending(int track) const noexcept { return (active_ >> (track & 15) & 1u) != 0; }
    int remaining(int track) const noexcept // repeats still to fire
    {
        const auto& p = pending_[static_cast<std::size_t>(track & 15)];
        return pending(track) ? p.count - p.next : 0;
    }
    // Frame of the next repeat relative to the current block's start (meaningful while pending()).
    std::int64_t nextFrame(int track) const noexcept { return pending_[static_cast<std::size_t>(track & 15)].nextAt - blockStart_; }
    std::int64_t clock() const noexcept { return blockStart_; }

private:
    struct Pending
    {
        std::int64_t origin = 0, nextAt = 0; // absolute frames: the step, the next repeat
        double step = 0.0, divisor = 1.0;    // repeat k at origin + round(k · step / divisor)
        std::uint8_t next = 0, count = 0;    // index of the next repeat (1 …), the step's count
        std::uint8_t velocity = 0;
        std::int8_t semitones = 0;
        trigger::TriggerKind kind = trigger::TriggerKind::Full;
        bool hasLocks = false;
    };
    struct Locks // the step's lock values, owned here while its repeats are pending
    {
        trigger::LockSet set {};
        std::array<float, kLockSlots> values;       // written before read (onStep), no braces (GCC 11 build time)
        std::array<std::uint8_t, kLockSlots> change; // idem
    };

    static std::int64_t offsetOf(const Pending& p, int k) noexcept
    {
        return static_cast<std::int64_t>(static_cast<double>(k) * p.step / p.divisor + 0.5);
    }

    std::array<Pending, kTracks> pending_; // default member initialisers; no braces (GCC 11 build time)
    std::array<Locks, kTracks> locks_;
    std::int64_t blockStart_ = 0;
    std::uint16_t active_ = 0;
};
} // namespace engine::sequencer::perf
