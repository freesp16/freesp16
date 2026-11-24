// Acceleration of the six knob-strip encoders (ENC1–6): a fast, sustained turn moves a value by more than one step
// per detent, so a long range needs a short turn; a slow or short turn stays one step per detent.
//
// Rules (all of them a pure function of the detents and their timestamps, so the same gesture always gives the same
// value: no timer, no wall clock, no floating-point state):
//   - A run is detents in one direction with less than kIdleMs between them. A reversal or a pause ends it, and the
//     next detent starts a new run at ×1: backing off an overshoot is always exact.
//   - The first kWarmDetents detents of a run are always ×1: a nudge of one or two detents is exact however fast.
//   - The speed is the mean time per detent, averaged over the run (EMA, ¼ weight per event, integer µs, starting
//     from a slow 60 ms), which absorbs the detents' mechanical unevenness. The speed sets the level ×2, ×4 or ×8
//     (kLevelUs); a turn held at the top speed then boosts it further, one level per kBoostDetents detents (×16, ×32,
//     ×64), for the long columns only (sample positions). The level rises at most one level per event; it falls as
//     soon as the speed no longer holds it, and holding a level needs only kHysteresisPercent of its threshold, so a
//     steady turn never flickers between two multipliers. A single detent over twice slower than its level's
//     threshold restarts the mean from it: slowing down gives the precision back on that very detent.
//   - The multiplier is capped by the column's span: a column of `steps` coarse steps accelerates to at most the
//     largest power of two ≤ steps / kSweepDetents (≥ 1), so even at full speed a full sweep takes about kSweepDetents
//     detents and stays controllable. Under 2 · kSweepDetents steps (enums, ON/OFF, small integers, a 0–24 range) a
//     column never accelerates; SHIFT (fine) never accelerates.
//
// Only the hardware panel's encoders accelerate (Shell::dispatch, io::kSourcePanel): the desktop's keys and mouse,
// MIDI controllers and touch drags keep one step per detent. Message thread; no allocation.
#pragma once

#include <array>
#include <cstdint>

namespace core
{
struct ParamSpec;
}

namespace ui
{
// One encoder event after acceleration bookkeeping. `scaled(maxMultiplier)` is the detents to apply.
struct EncoderTurn
{
    int detents = 0; // as turned (signed)
    int warm = 0;    // how many of them are past the run's warm-up (0 … |detents|)
    int level = 0;   // multiplier 1 << level before the column's cap

    int multiplier(int maxMultiplier) const noexcept
    {
        const int m = 1 << level;
        return m < maxMultiplier ? m : (maxMultiplier < 1 ? 1 : maxMultiplier);
    }
    // The warm-up detents count once, the others × the multiplier; the sign follows the turn.
    int scaled(int maxMultiplier) const noexcept
    {
        const int n = detents < 0 ? -detents : detents;
        const int out = (n - warm) + warm * multiplier(maxMultiplier);
        return detents < 0 ? -out : out;
    }
};

class EncoderAccel
{
public:
    static constexpr int kEncoders = 6;
    static constexpr int kWarmDetents = 3;        // ×1 at the start of every run
    static constexpr std::int64_t kIdleUs = 120'000; // a longer pause ends the run
    static constexpr int kSweepDetents = 24;      // ≈ one turn of the encoder: a full sweep at the top speed
    static constexpr int kSpeedLevels = 3;        // ×8 from the speed alone
    static constexpr int kMaxLevel = 6;           // ×64 with the boost
    static constexpr int kBoostDetents = 8;       // detents held at the top speed per boost level
    // Mean µs per detent at which level 1…kSpeedLevels starts (25, 40, 67 detents per second).
    static constexpr std::array<std::int64_t, kSpeedLevels + 1> kLevelUs {0, 40'000, 25'000, 15'000};
    static constexpr int kHysteresisPercent = 125; // a level held so far holds until the mean is 25 % slower

    // Books one Turn event of encoder `n0` (0–5) at `timeMs` (the event's own timestamp). Out-of-range encoders and
    // zero detents return a plain ×1 turn and leave the state alone.
    EncoderTurn turn(int n0, int detents, double timeMs) noexcept;
    void reset(int n0) noexcept;
    void reset() noexcept;

    // The cap for a column of `steps` coarse steps (0 = not accelerated): 1, 2, 4 … 64.
    static int maxMultiplier(long long steps) noexcept;
    // A parameter column's span in coarse steps for maxMultiplier(); 0 for enums, ON/OFF and with SHIFT (fine).
    static long long stepsOf(const core::ParamSpec&, bool fine) noexcept;

private:
    struct Run
    {
        bool active = false;
        std::int64_t lastUs = 0;
        int dir = 0;
        int count = 0;          // detents in the run so far (saturates)
        std::int64_t meanUs = 0; // 0 = no interval measured yet
        int level = 0;
        int atLevel = 0;         // detents since the level last changed (saturates)
    };
    std::array<Run, kEncoders> runs_ {};
};
} // namespace ui
