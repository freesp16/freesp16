// Step 6 of the engine block (audio-engine.md "Per-block processing order"; spec
// 02-features/sequencer.md): plays the current pattern of the current scene for all 16 tracks and records live hits.
//
//   timing     step s of a pattern sits at patternStartPpq + s × 0.25 (SEQ-002/003); odd steps (2nd, 4th, …) are
//              delayed by swing × 0.25 (SEQ-004, swing01 0–0.5 = 0–50 % shown); the step's offset — its OFF SET lock,
//              else the track's offset — moves it by offset24 / 24 step (SEQ-005). collect() scans only the grid steps
//              that can land in [p0, p1) (≤ 1 step early, ≤ swing + 1 step late: 2–4 per block) and emits those whose
//              final time is in [p0, p1): half-open, so every step fires exactly once per pass, step 1 included on
//              start (SEQ-014). A negative offset on step 1 fires at the end of the previous pass; nothing fires
//              before the pattern's first pass. Frame = scene::frameAt(t): sample-accurate offline (SEQ-060).
//   passes     timing.patternStartPpq is the start of the pass that holds p0 (the switcher moves it at every wrap): the
//              sequencer counts the passes of a run (Where::pass) so "recorded this pass" and a late offset across the
//              wrap ("pass −1" of the block after it) stay exact; a new pattern, a start edge or a locate begins a run.
//   switch     the boundary contract of engine/scene/ScenePatternSwitcher.h: with timing.boundary in the block the old
//              pattern (the previous block's, kept here) plays [p0, B) and frames [0, boundary.frame), the new one
//              (timing.*) [B, p1); RESTART / CONTINUE are the switcher's patternStartPpq; stopped or counting in, the
//              switch is immediate (no boundary). Every frame is scene::frameAt, the switcher's rounding.
//   events     FULL → TriggerKind::Full, HALF → Half, PARAM → LockOnly (SEQ-011), source Sequencer (TRIGGER MODE is
//              ignored, SEQ-015, by the track source), ducks for Full / Half (SEQ-027), the cell's LockSet by pointer
//              (35 slots, SEQ-020; owned by the installed PatternData, alive ≥ 2 blocks after a swap). Consumers apply
//              locks at the trigger and revert / hold at the track's next event (SEQ-021, TrackSource / AmpEnvelope /
//              Mixer::applyLocks).
//   recording  armed (`seq.record`) and playing: every live Full pad / MIDI hit (the router's LiveHitSink, PAD-005,
//              SEQ-041/048) — and every touch strip REPEAT hit of the running pass (origin kOriginRepeat, ours,
//              RIB-008) — is placed at its exact PPQ, quantised (core/util/sequencer/RecordQuantize.h, SEQ-042) and
//              sent to the UI as EngineEvent::RecordedHit {track, step, velocity, offset24; a MIDI note's, SCALE
//              pad's or its REPEAT's semitones} (lossless queue); the message thread writes it with recordHit inside
//              the pass's CommandGroup (EDT-U3), the semitones as the step's PITCH lock. A hit whose stored trigger
//              plays later in the pass sets a per-track "recorded this pass" bit, so that step does not sound a second
//              time (the live hit already did) whether or not the new pattern object arrived. ERASE held
//              (`seq.erase`, SEQ-046) removes the track's triggers as the playhead passes: EngineEvent::ErasedStep,
//              the step is not played. Pre-count blocks (timing.preCount) play nothing; a hit late in the count-in that
//              quantises onto step 1 is recorded there (SEQ-044, our choice), any other count-in hit is not.
//              padStartsSequence (SEQ-047): the first hit while stopped and armed is recorded on step 1 and asks the
//              transport to start (takeStartRequest, read by the engine after the router).
//
// Budget: collect for 16 tracks with locks, swing and record capture ≤ 15 µs per block on the
// A9 (tests/bench/bench_sequencer.cpp). Index arithmetic only, no search, no allocation, lock sets by pointer.
#pragma once

#include "core/util/sequencer/RecordQuantize.h"
#include "engine/EngineApi.h"
#include "engine/feeds/Feeds.h"
#include "engine/sequencer/PatternData.h"
#include "engine/stages/BlockBuffers.h"
#include "engine/trigger/TriggerRouter.h"

#include <array>
#include <cstdint>

namespace engine::sequencer
{
using core::sequencer::RecordQuantize;

struct RecordPitch // a pitched hit's semitones (EngineEvent::pitched / semitones)
{
    bool pitched = false;
    std::int8_t semitones = 0;
};

class Sequencer final : public trigger::LiveHitSink
{
public:
    static constexpr int kScenes = 16, kPatterns = 16;
    static constexpr double kStepPpq = 0.25; // core::musictime::kQuartersPerStep (SEQ-002)

    Sequencer() noexcept;
    Sequencer(const Sequencer&) = delete;
    Sequencer& operator= (const Sequencer&) = delete;

    // ---- message thread (set-up) ----
    void prepare(double sampleRate) noexcept;
    void setEventQueue(feeds::EngineFeeds* feeds) noexcept { feeds_ = feeds; } // EngineEvents out (null = none)

    // ---- audio thread ----
    // The object installed in SwapSlot::Pattern(scene, pattern) (the stage mirrors the swap table; the engine's swap
    // protocol retires the old one).
    void install(int scene, int pattern, const PatternData* p) noexcept { patterns_[scene & 15][pattern & 15] = p; }
    const PatternData* installed(int scene, int pattern) const noexcept { return patterns_[scene & 15][pattern & 15]; }
    void setSwing(float swing01) noexcept; // 0–0.5 step (SWING 0–50 %, SEQ-004)
    void setTrackOffset(int scene, int track, std::int8_t offset24) noexcept;
    void setRecord(bool armed, RecordQuantize q, bool padStartsSequence) noexcept
    {
        setRecordArmed(armed);
        quantize_ = q;
        padStarts_ = padStartsSequence;
    }
    void setRecordArmed(bool armed) noexcept;
    void setRecordQuantize(RecordQuantize q) noexcept { quantize_ = q; }
    void setPadStartsSequence(bool on) noexcept { padStarts_ = on; }
    void setErase(int track, bool held) noexcept; // SEQ-046

    // Emits every step event whose time ∈ [p0, p1), split at the switch boundary (old pattern before, new after).
    // `timing` = BlockBuffers::timing (scene / pattern / patternStartPpq / boundary / preCount, from the switcher and
    // the transport; the frozen BlockContext cannot carry them).
    void collect(const BlockContext&, const stages::BlockBuffers::Timing& timing, trigger::TriggerRouter&) noexcept;
    void onLiveHit(const trigger::TriggerEvent&, bool pitched) noexcept override; // record capture → EngineEvent
    bool takeStartRequest() noexcept // padStartsSequence: the engine posts Play to the transport
    {
        const bool r = startRequest_;
        startRequest_ = false;
        return r;
    }

    // ---- state (audio thread; tests after the block) ----
    int playStep() const noexcept { return playStep_; } // 0–63 of the current pattern at p0 (PlayheadFeed::step)
    bool armed() const noexcept { return armed_; }
    float swing() const noexcept { return static_cast<float>(swing_ / kStepPpq); }
    RecordQuantize recordQuantize() const noexcept { return quantize_; }
    std::uint64_t recordedThisPass(int track) const noexcept
    {
        return suppress_[static_cast<std::size_t>(track & 15)] | suppressNext_[static_cast<std::size_t>(track & 15)];
    }
    int eventsThisBlock() const noexcept { return pushed_; }
    std::uint64_t routerDropped() const noexcept { return routerDropped_; }

private:
    // The pattern a segment plays: `start` = timing.patternStartPpq (the start of the pass that holds p0; the switcher
    // moves it at every wrap), `pass` = that pass's number since the run began (the "recorded this pass" bits are
    // keyed by it), `first` = no earlier pass of this run exists (a step of "pass −1" — a late offset across the wrap —
    // only fires when it is false).
    struct Where
    {
        int scene = 0, pattern = 0;
        double start = 0.0;
        long long pass = 0;
        bool first = true;
    };
    Where follow(const BlockContext& ctx, const stages::BlockBuffers::Timing& timing) const noexcept;
    void emit(const Where& w, double a, double b, bool current, trigger::TriggerRouter& router, int frameEnd) noexcept;
    void record(int track, int velocity, double hitPpq, const Where& w, bool current,
                std::uint8_t origin = EngineEvent::kOriginLive, RecordPitch pitch = {}) noexcept;
    void push(EngineEvent::Kind kind, int track, int step, int value, int offset24,
              std::uint8_t origin = EngineEvent::kOriginLive, RecordPitch pitch = {}) noexcept;

    feeds::EngineFeeds* feeds_ = nullptr;
    std::array<std::array<const PatternData*, kPatterns>, kScenes> patterns_ {};
    std::array<std::array<std::int8_t, kTracks>, kScenes> trackOffset_ {};
    double swing_ = 0.0; // in PPQ
    double sampleRate_ = 44100.0;
    RecordQuantize quantize_ = core::sequencer::kDefaultRecordQuantize;
    bool armed_ = false, padStarts_ = false, startRequest_ = false;
    std::uint16_t erase_ = 0;
    // "Recorded this pass": bit s = step s of the track must not sound again — for grid pass supBase_ (suppress_) and
    // the one after it (suppressNext_: a hit quantised onto the next pass's step 1).
    std::array<std::uint64_t, kTracks> suppress_ {}, suppressNext_ {};
    long long supBase_ = 0;
    void clearSuppress() noexcept;
    void advancePass(long long pass) noexcept;

    // The block being processed (collect → onLiveHit, same block).
    bool playing_ = false, preCount_ = false, hasPrev_ = false, boundary_ = false;
    int boundaryFrame_ = 0;
    double p0_ = 0.0, ppf_ = 0.0;
    Where cur_ {}, prev_ {};
    int playStep_ = 0, pushed_ = 0;
    std::uint64_t routerDropped_ = 0;
};
} // namespace engine::sequencer
