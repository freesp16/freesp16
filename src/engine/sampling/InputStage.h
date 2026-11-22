// engine::sampling::InputStage — the input stage (spec 02-features/sampling.md SMP-003/004,
// mixer-and-routing.md MIX-021, audio-engine.md "Reference signal flow": INPUT LEVEL → SOFT LIMIT on/off → input
// meter → SAMPLING source | Thru tracks | REC MONITOR).
//
//   raw input (BlockBuffers::inputL/R, one block late) → clip detect (raw |x| ≥ kClipLevel) → soft limiter (on/off,
//   default OFF: engine::mixer::SoftLimiter, transparent ≤ −3 dBFS, soft knee to the −0.3 dBFS ceiling, NEON) → peak
//   meter → BlockBuffers::inputL/R again. Everything after it — Thru tracks (8a), the input monitor,
//   the MeterFeed input peak, the sampler's LINE IN source — reads the post-limiter input.
//
// Limiter OFF (the default) or the block below −3 dBFS: no copy, the pointers stay the raw input (the shaper is
// identity there). Limiter ON and a louder block: one copy of 2 × 64 floats into this stage's buffers, then the shaper.
//
// Threads: audio thread only (setLimiter from the step-1 drain); no allocation. Budget: limiter ≤ 300,
// meter ≤ 60 A9 cycles per block.
#pragma once

#include "engine/EngineApi.h"
#include "engine/mixer/Mixer.h"
#include "engine/stages/BlockBuffers.h"

namespace engine::sampling
{
class InputStage
{
public:
    static constexpr float kClipLevel = 0.9999f; // S16 full scale is 32767/32768 = 0.99997

    void setLimiter(bool on) noexcept { limiter_on_ = on; }
    bool limiter() const noexcept { return limiter_on_; }

    // Step 1½ (after the FIFO drain, before 8a): b.inputL/R → the post-limiter input. No input: peaks 0.
    void process(stages::BlockBuffers& b) noexcept;

    float peak(int ch) const noexcept { return peak_[ch & 1]; } // post-limiter, this block
    bool clipped() const noexcept { return clip_; }             // raw input reached full scale this block
    const mixer::SoftLimiter& shaper() const noexcept { return limiter_; }

private:
    mixer::SoftLimiter limiter_; // builds its table in the constructor (message thread, with the engine)
    bool limiter_on_ = false;
    bool clip_ = false;
    float peak_[2] {};
    alignas(16) float l_[kBlockFrames] {};
    alignas(16) float r_[kBlockFrames] {};
};
} // namespace engine::sampling
