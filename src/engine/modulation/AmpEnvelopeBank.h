// The AHR amp envelopes of the 16 tracks as structure-of-arrays (spec 02-features/amp-envelope.md ENV-001…011).
// One AmpEnvelopeBank lives in the engine next to the track sources; the TrackSource stage drives it per block:
//
//   message thread  bank.prepare(sampleRate)                              tables + reset (libm here only)
//   audio thread    FIFO drain:   applyParam(params[scene][t], field, v); bank.setParams(t, params[scene][t], ctx.bpm)
//                   router:       bank.trigger(event, gate)  for every TriggerEvent of track event.track, in frame
//                                 order (Full / Half / LockOnly / Release / Stop; gate = pad in GATE mode, false for
//                                 sequencer steps, TRK-011); bank.release(t, frame) = GATE pad up; bank.kill(t, frame)
//                   track source: voice.render(l, r) → bank.process(t, l, r)  (64 frames, in place, NEON)
//                                 bank.finished(t) → stop the voice (and leave the track inactive)
//                                 the voice ended on its own (non-looping sample end) → bank.reset(t): HOLD = INF,
//                                 RELEASE = INF and bypass never finish by themselves ("the voice ends with the sample")
//                   Thru tracks:  bank.setThru(t, true): no envelope; process() is then a no-op
//
// Events are sample-accurate: trigger()/release()/kill() of a block are queued per track (≤ kMaxEvents) and applied
// at their frame inside the next process(); before that frame the previous state continues. A restart (FULL, or HALF
// and retrigger repeats with Retrigger on, ENV-007) starts the attack from the current level, so it never clicks.
//
// Gain curves (ENV-004): LINEAR = straight ramps; CURVE = attack g = P − (P − g0)·ρ^n reaching −60 dB residual at the
// attack time then landing on P, release g = L·ρ^n reaching −80 dB at the release time then cut. Each segment's last
// sample lands exactly on its target. Per block a track costs one gain fill per segment run (NEON: 4 lanes seeded
// {g0ρ, g0ρ², g0ρ³, g0ρ⁴}, stepped by ρ⁴; linear runs are index-based, a + d·k) and one stereo multiply; a constant
// segment (hold, sustain, idle, bypass) costs one scalar multiply or nothing at unity. Geometric state is carried in
// double between runs (ρ^m by binary powering, no libm), so 32 s curves keep their closed form within 1e-5.
//
// Details of our own: a running segment keeps its coefficients when a parameter changes (the change
// applies from the next segment or trigger), except BYPASS, which a sounding note follows at once (setParams: on → the
// 2 ms ramp to unity; off → the segment the note is in now, the peak for what is left of its HOLD, else its release
// from the current level; a released GATE note, a kill and a bypass fade-out finish as they are); release lengths
// have a 32-frame floor (RELEASE 0 included); an attack restarting from a non-zero level has the same floor; a release
// from level 0 ends at once; HALF on an idle track does nothing; LockOnly / non-restarting HALF update only the RELEASE
// lock of a running envelope.
//
// Modulation (ModMatrix destinations env.velocity / attack / hold / release, via setModulation): applied on top of the
// stored or locked value where the field is read — VELOCITY, ATTACK, HOLD at a restart, RELEASE when the release
// starts — so an LFO or a touch strip USER item also acts on a step that locks the field (FX-084: "base is the
// stored or locked value"); setParams keeps the stored values unmodulated.
#pragma once

#include "engine/EngineApi.h"
#include "engine/modulation/AmpEnvelopeKernels.h"
#include "engine/modulation/ModMatrix.h"
#include "engine/modulation/AmpEnvelopeParams.h"
#include "engine/modulation/AmpEnvelopeTables.h"
#include "engine/trigger/TriggerEvent.h"

#include <array>
#include <cstdint>

namespace engine::modulation
{
enum class AmpEnvStage : std::uint8_t
{
    Idle,      // silent, finished
    Attack,    // current level → peak
    Hold,      // at the peak until `hold` after the trigger (INFINITY: until gate release / the sample ends)
    Release,   // → 0 (RELEASE = INFINITY: stays at the level reached, never finishes)
    Kill,      // 64-frame de-click to 0 (choke, panic, Stop)
    BypassIn,  // ENV-009: 2 ms ramp to 1
    Bypass,    // gain 1 while the voice plays
    BypassOut, // 2 ms ramp to 0 (GATE release while bypassed)
    Thru       // Thru track: no envelope at all, process() is a no-op
};

template <int N> class AmpEnvelopeLanes
{
public:
    static_assert(N >= 1 && N <= 64);
    static constexpr int kLanes = N;
    static constexpr int kFrames = kBlockFrames;
    static constexpr int kMaxEvents = 8; // per track and block; a full queue keeps the latest events

    void prepare(double sampleRate) noexcept; // message thread: tables + every lane idle with ENV-011 defaults
    void setTempo(double bpm) noexcept;       // sync lengths; recomputes only when bpm changed (≤ once per block)
    void setParams(int lane, const AmpEnvParams&, double bpm) noexcept;
    const AmpEnvParams& params(int lane) const noexcept { return base_[static_cast<std::size_t>(lane)]; }
    // The matrix's modulation of VELOCITY / ATTACK / HOLD / RELEASE (other fields: ignored); null = unmodulated.
    void setModulation(int lane, AmpEnvField field, const ModValue* v) noexcept;
    // The index `field` plays at for a stored or locked `index` (the modulation applied; tests).
    int modulated(int lane, AmpEnvField field, int index) const noexcept;
    void setThru(int lane, bool thru) noexcept;
    void useScalarKernels(bool scalar) noexcept; // tests: the scalar references instead of NEON

    // Queue an event of this block (lane = event.track).
    void trigger(const trigger::TriggerEvent&, bool gate) noexcept;
    void release(int lane, int frame) noexcept;
    void kill(int lane, int frame) noexcept;
    void reset(int lane) noexcept; // immediately idle, pending events dropped (no ramp: the voice is already gone)

    float currentGain(int lane) const noexcept { return level_[static_cast<std::size_t>(lane)]; }
    bool finished(int lane) const noexcept;
    AmpEnvStage stage(int lane) const noexcept
    {
        return static_cast<AmpEnvStage>(stage_[static_cast<std::size_t>(lane)]);
    }

    // One block: writes the 64 gains (advancing the envelope), or multiplies l and r in place by them.
    void renderGain(int lane, float* gain) noexcept;
    void process(int lane, float* l, float* r) noexcept;

private:
    enum Shape : std::uint8_t
    {
        Const,
        Linear,
        Geo
    };
    enum EventType : std::uint8_t
    {
        EvTrigger,
        EvRelease,
        EvKill
    };
    struct Event
    {
        std::uint8_t frame = 0, type = EvTrigger, velocity = 127;
        trigger::TriggerKind kind = trigger::TriggerKind::Full;
        trigger::TriggerSource source = trigger::TriggerSource::Pad;
        bool gate = false;
        std::int16_t lockVelocity = -1, lockAttack = -1, lockHold = -1, lockRelease = -1; // −1 = not locked
    };
    static constexpr std::int32_t kNoHold = -2; // attack cut short by hold: release follows directly

    void enqueue(int lane, const Event&) noexcept;
    void applyEvent(int lane, const Event&) noexcept;
    void restart(int lane, const Event&) noexcept;
    void renderRun(int lane, float* g, int n) noexcept;
    bool constantBlock(int lane, float& value) const noexcept;
    void advanceConstant(int lane, int n) noexcept;
    void enterNext(int lane) noexcept;
    void settle(int lane) noexcept;
    void setConst(int lane, AmpEnvStage, float value, std::int32_t frames) noexcept;
    void setLinear(int lane, AmpEnvStage, float from, float to, std::int32_t frames) noexcept;
    void setGeo(int lane, AmpEnvStage, float asymptote, double term, double rho, float target, std::int32_t frames,
                bool snap) noexcept;
    void startAttack(int lane, float peak, AmpEnvTables::Segment attack, std::int32_t hold) noexcept;
    void startRelease(int lane) noexcept;
    void bypassChanged(int lane) noexcept;

    AmpEnvTables tables_ {};
    const envk::KernelSet* k_ = &envk::activeKernels();
    std::array<float, 128> peakGain_ {}; // core::GainLaw::gain(i), copied in prepare()

    // Segment state, one entry per lane (structure of arrays).
    std::array<std::uint8_t, N> stage_ {}, shape_ {};
    std::array<bool, N> snap_ {}, gated_ {};
    std::array<float, N> a_ {}, d_ {}, target_ {}, level_ {}, peak_ {};
    std::array<double, N> term_ {};
    std::array<std::array<double, N>, 7> rhoPow_ {}; // ρ^(2^b), b = 0…6 (runs are ≤ 64 = 2^6 frames)
    std::array<std::int32_t, N> pos_ {}, len_ {}, holdAfter_ {};
    std::array<std::int16_t, N> lockedRelease_ {};
    // Since the last restart, for a live BYPASS off: frames (block-exact, saturating), HOLD end (kInfinite = INF),
    // the note's gate mode (gated_ is cleared by the release).
    std::array<std::int32_t, N> age_ {}, holdEnd_ {};
    std::array<bool, N> gateNote_ {};
    // Parameters and the queued events of this block.
    std::array<AmpEnvParams, N> base_ {};
    std::array<std::array<ModValue, 4>, N> mod_ {}; // per lane: Velocity, Attack, Hold, Release
    std::array<std::uint8_t, N> modMask_ {};         // bit f = field f is modulated
    std::array<std::uint8_t, N> events_ {};
    std::array<std::array<Event, kMaxEvents>, N> queue_ {};
};

using AmpEnvelopeBank = AmpEnvelopeLanes<kTracks>;

extern template class AmpEnvelopeLanes<1>;
extern template class AmpEnvelopeLanes<kTracks>;
} // namespace engine::modulation
