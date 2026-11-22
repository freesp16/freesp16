// The mixer's ramps, operation for operation the research model's state machines (tools/fx/track_mix.py `Smoother` and
// `MuteSmoother`, MIT; docs/effects/signal-flow.md "Track strip"): MIX-002 — fader, send, pan, return and master
// changes ramp linearly over 100 samples, mute over 128.
//
//   GainRamp  step = (target − current) × (1/100), set once per change; the ramp starts at the change's frame (the
//             value is held until then), crosses block boundaries and lands exactly on the target. A new target
//             mid-ramp starts from the value reached; the same target again changes nothing.
//   MuteRamp  128-sample linear fade (step ±1/128); a change mid-fade turns round from the gain reached (counter =
//             128 − counter). Closed = the strip feeds nothing. A change at frame k > 0 takes effect at frame k
//             (the frames before it keep the previous state — the model only switches at block starts); one change
//             per block (the last one) is kept.
// All real-time safe (no allocation, no libm); the arithmetic is float with one rounding per operation.
#pragma once

#include <cstdint>

namespace engine::mixer
{
class GainRamp
{
public:
    static constexpr int kLength = 100;
    static inline const float kInv = static_cast<float>(1.0 / kLength); // F(1 / length), as the model

    void reset(float value) noexcept
    {
        cur_ = target_ = value;
        step_ = 0.0f;
        count_ = offset_ = 0;
    }
    void setTarget(float t, int frame) noexcept
    {
        if (target_ == t)
            return;
        const float diff = t - cur_;
        step_ = diff * kInv;
        target_ = t;
        count_ = kLength;
        offset_ = frame > 0 ? frame : 0;
    }
    bool idle() const noexcept { return count_ == 0; }
    float current() const noexcept { return cur_; }
    float target() const noexcept { return target_; }

    // out[i] = the gain of frame i (null: advance only — the sequential adds are kept, so the state stays exact).
    void fill(float* out, int n) noexcept
    {
        int i = 0;
        if (count_ == 0)
        {
            if (out != nullptr)
                for (; i < n; ++i)
                    out[i] = cur_;
            return;
        }
        const int start = offset_ < n ? offset_ : n;
        if (out != nullptr)
            for (; i < start; ++i)
                out[i] = cur_;
        if (offset_ >= n)
        {
            offset_ -= n;
            return;
        }
        offset_ = 0;
        i = start;
        float g = cur_;
        const int k = count_ < n - start ? count_ : n - start;
        for (int j = 0; j < k; ++j, ++i)
        {
            if (out != nullptr)
                out[i] = g;
            g = g + step_;
        }
        if (count_ > k)
        {
            count_ -= k;
            cur_ = g;
            return;
        }
        cur_ = target_;
        count_ = 0;
        step_ = 0.0f;
        if (out != nullptr)
            for (; i < n; ++i)
                out[i] = cur_;
    }

    // The 4-lane form used by the strips (kernels::Ramp4): start/end of the ramp inside an n-frame block.
    int startIn(int n) const noexcept { return count_ == 0 ? 0 : (offset_ < n ? offset_ : n); }
    int endIn(int n) const noexcept
    {
        if (count_ == 0)
            return 0;
        const long e = static_cast<long>(offset_) + count_;
        return e < n ? static_cast<int>(e) : n + 1; // n + 1: still ramping after the block
    }
    float step() const noexcept { return step_; }
    // After a 4-lane fill of n frames: `acc` is the lane's running value at the end of the block.
    void finishBlock(float acc, int n) noexcept
    {
        if (count_ == 0)
            return;
        if (offset_ >= n)
        {
            offset_ -= n;
            return;
        }
        const int consumed = n - offset_;
        offset_ = 0;
        if (count_ > consumed)
        {
            count_ -= consumed;
            cur_ = acc;
            return;
        }
        cur_ = target_;
        count_ = 0;
        step_ = 0.0f;
    }

private:
    float cur_ = 1.0f, target_ = 1.0f, step_ = 0.0f;
    int count_ = 0, offset_ = 0;
};

class MuteRamp
{
public:
    static constexpr int kLength = 128;
    static inline const float kInv = static_cast<float>(1.0 / kLength);
    enum class Kind : std::uint8_t
    {
        Open,   // gain 1 for the whole block (no multiply)
        Closed, // gain 0 for the whole block (the strip feeds nothing)
        Buffer  // per-frame gains written
    };

    void reset(bool muted) noexcept
    {
        muted_ = muted;
        counter_ = 0;
        state_ = muted ? State::Closed : State::Open;
        g_ = muted ? 0.0f : 1.0f;
        step_ = muted ? kInv : -kInv;
        pending_ = false;
    }
    bool muted() const noexcept { return pending_ ? pendingMuted_ : muted_; }
    void set(bool muted, int frame) noexcept
    {
        if (frame <= 0)
        {
            pending_ = false;
            flip(muted);
            return;
        }
        pending_ = true;
        pendingMuted_ = muted;
        pendingFrame_ = frame;
    }
    // Hard mute: gain 0 at once, no ramp (audio-engine.md "Hard mute").
    void cut() noexcept
    {
        pending_ = false;
        muted_ = true;
        state_ = State::Closed;
        g_ = 0.0f;
        counter_ = 0;
        step_ = kInv;
    }
    bool settled() const noexcept { return ! pending_ && state_ != State::Ramping; }

    // The block's mute gains; out may be null (advance only).
    Kind fill(float* out, int n) noexcept
    {
        if (! pending_)
        {
            if (state_ == State::Open)
                return Kind::Open;
            if (state_ == State::Closed)
                return Kind::Closed;
            segment(out, 0, n);
            return Kind::Buffer;
        }
        const int k = pendingFrame_ < n ? pendingFrame_ : n;
        segment(out, 0, k);
        if (pendingFrame_ >= n)
        {
            pendingFrame_ -= n;
            return Kind::Buffer;
        }
        pending_ = false;
        flip(pendingMuted_);
        segment(out, k, n);
        return Kind::Buffer;
    }

private:
    enum class State : std::uint8_t
    {
        Open,
        Ramping,
        Closed
    };

    void flip(bool m) noexcept // MuteSmoother.set_muted
    {
        if (muted_ == m)
            return;
        step_ = m ? -kInv : kInv;
        muted_ = m;
        counter_ = kLength - counter_;
        state_ = State::Ramping;
    }
    void segment(float* out, int pos, int end) noexcept // MuteSmoother.apply over [pos, end)
    {
        while (pos < end)
        {
            if (state_ == State::Open || state_ == State::Closed)
            {
                if (out != nullptr)
                {
                    const float v = state_ == State::Open ? 1.0f : 0.0f;
                    for (; pos < end; ++pos)
                        out[pos] = v;
                }
                return;
            }
            const int len = end - pos;
            const int k = counter_ >= len ? len : counter_;
            float v = g_;
            for (int i = 0; i < k; ++i)
            {
                if (out != nullptr)
                    out[pos + i] = v;
                v = v + step_;
            }
            if (counter_ >= len)
            {
                counter_ -= len;
                g_ = v;
                return;
            }
            pos += k;
            counter_ = 0;
            state_ = muted_ ? State::Closed : State::Open;
            g_ = muted_ ? 0.0f : 1.0f;
        }
    }

    float g_ = 1.0f, step_ = -kInv;
    int counter_ = 0;
    State state_ = State::Open;
    bool muted_ = false;
    bool pending_ = false, pendingMuted_ = false;
    int pendingFrame_ = 0;
};
} // namespace engine::mixer
