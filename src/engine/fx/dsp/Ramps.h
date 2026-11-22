// Gain ramps of the effects framework (DSP kit), re-implemented from the research analysis
// (docs/effects/effectbase.md "SmoothFloat", fx-processors.md "MuteSmoother", CC BY 4.0) and operation for operation
// equal to the MIT models tools/fx/effectbase.py (SmoothFloat), lofi.py (Smooth, SignalGate) and fx_processors.py
// (MuteSmoother):
//
//   SmoothFloat  accumulated linear ramp over N samples: the first sample is the start value, then
//                value_t = f32(value_{t−1} + step), step = f32(f32(target − current) · f32(1/N)); at the end the value
//                snaps to the target. setTime() on a running ramp restarts it from the current value (reference).
//   SignalGate   a 0/1 gate on a SmoothFloat (the post-stage gates around the 10 Hz high-pass).
//   MuteSmoother the N_mute mute ramp of the Send input and the Insert bypass crossfade
//                out = f32(f32(dry·(1 − g)) + f32(g·processed)).
//
// All block functions are real-time safe (no allocation, bounded loops) and take n <= kMaxBlock samples.
#pragma once

#include "engine/fx/dsp/Numerics.h"

namespace engine::fx::dsp
{
constexpr int kMaxBlock = 64;

class SmoothFloat
{
public:
    void prepare(double fs) noexcept;               // N = max(trunc(f32(fs) · 0.0033333332f), 1) (reference ctor)
    void prepareSamples(double fs, int n) noexcept; // explicit N (lofi.py Smooth / SignalGate)
    void setTime(float ms) noexcept;                // setSmoothingTime: N = trunc(f32(ms · 0.001) · fs)
    void snap(float v) noexcept;                    // jump, no ramp
    void setTarget(float v) noexcept;               // ignored when near the current target
    // Per-sample gains of the next n samples into g (advances the ramp). Only meaningful while ramping().
    void block(float* g, int n) noexcept;
    // The reference "multiply both channels by this gain" helper: constant |g − 1| < ε skips, |g| < ε zero-fills.
    void apply(float* l, float* r, int n) noexcept;
    // Steady value with the reference shortcuts applied (1 when near 1, 0 when |v| < ε): x·steady() equals what
    // apply() leaves, except for the sign of a zero.
    float steady() const noexcept;

    bool ramping() const noexcept { return state_ != 0; }
    float current() const noexcept { return cur_; }
    float target() const noexcept { return target_; }
    int samples() const noexcept { return n_; }
    int remaining() const noexcept { return rem_; }

private:
    double fs_ = 44100.0;
    int state_ = 0;
    float cur_ = 0.0f, target_ = 0.0f, step_ = 0.0f;
    int rem_ = 0;
    int n_ = 1;
    float inv_ = 1.0f;
};

class SignalGate
{
public:
    void prepare(double fs, int n) noexcept; // starts closed
    void open() noexcept { ramp_.setTarget(1.0f); }
    void close() noexcept { ramp_.setTarget(0.0f); }
    void setNow(bool on) noexcept { ramp_.snap(on ? 1.0f : 0.0f); }
    void apply(float* l, float* r, int n) noexcept;
    // Closed and settled: apply() would zero the block (the post-stage skips the gates and the filter then).
    bool settledClosed() const noexcept { return ! ramp_.ramping() && nearlyEqual(ramp_.current(), 0.0f); }

private:
    SmoothFloat ramp_;
};

class MuteSmoother
{
public:
    enum State : std::uint8_t
    {
        Open = 0,
        Ramp = 1,
        Closed = 2
    };
    void prepare(int n, bool muted) noexcept;
    void setMuted(bool m) noexcept;
    void process(float* l, float* r, int n) noexcept;                                     // in place
    void crossfade(float* l, float* r, const float* dl, const float* dr, int n) noexcept; // processed ↔ dry
    State state() const noexcept { return static_cast<State>(state_); }
    bool muted() const noexcept { return muted_; }

private:
    bool muted_ = false;
    int n_ = 1, rem_ = 0;
    float inv_ = 1.0f, step_ = -1.0f, g_ = 1.0f;
    std::uint8_t state_ = Open;
};
} // namespace engine::fx::dsp
