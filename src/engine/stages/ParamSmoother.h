// Click-free parameter changes (NFR-031: no clicks on mute, fader, … "all ramps as specified"; audio-engine.md: fader
// 100-sample linear ramp per change, mute 128-sample ramp). A linear ramp from the current value to the target over
// `rampSamples` samples, sample-accurate across block boundaries; a new target mid-ramp starts from the value reached
// (no discontinuity). The ramp ends exactly on the target (no float drift), so a gain of 0 is exact silence and a gain
// of 1 is bit-transparent afterwards (NFR-030).
//
// Real-time: fill() writes one gain per sample into a block buffer (scalar recurrence, one add per sample); while not
// ramping, isConstant() lets the caller use a single scalar multiply instead.
#pragma once

#include <cstdint>

namespace engine::stages
{
class ParamSmoother
{
public:
    // Our floor for every ramp (rampSamples 0…31 included): a gain step is never applied faster than 32 samples
    // (0.73 ms at 44.1 kHz), so even a sender's "no ramp" cannot click (NFR-031).
    static constexpr int kMinRamp = 32;

    void reset(float value) noexcept
    {
        current_ = target_ = value;
        step_ = 0.0f;
        remaining_ = 0;
    }
    void setTarget(float target, int rampSamples) noexcept
    {
        if (target == target_ && remaining_ == 0)
            return;
        target_ = target;
        remaining_ = rampSamples > kMinRamp ? rampSamples : kMinRamp;
        step_ = (target_ - current_) / static_cast<float>(remaining_);
    }
    bool isConstant() const noexcept { return remaining_ == 0; }
    float current() const noexcept { return current_; }
    float target() const noexcept { return target_; }

    // Writes n per-sample values (the value *after* each sample's step) and advances.
    void fill(float* out, int n) noexcept
    {
        int i = 0;
        float v = current_;
        for (; i < n && remaining_ > 0; ++i)
        {
            if (--remaining_ == 0)
                v = target_; // land exactly on the target
            else
                v += step_;
            out[i] = v;
        }
        for (; i < n; ++i)
            out[i] = v;
        current_ = v;
    }
    // Advances n samples without writing (a silent stage still tracks its ramp).
    void skip(int n) noexcept
    {
        if (remaining_ == 0)
            return;
        if (n >= remaining_)
        {
            current_ = target_;
            remaining_ = 0;
            return;
        }
        current_ += step_ * static_cast<float>(n);
        remaining_ -= n;
    }

private:
    float current_ = 0.0f, target_ = 0.0f, step_ = 0.0f;
    int remaining_ = 0;
};
} // namespace engine::stages
