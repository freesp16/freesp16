// Slice-point analysis (spec 02-features/slicing.md SLC-006 "zero-crossing snap"; budget
// ≤ 0.1 ms for the ±5 ms window on the A9, NEON sign-change scan). Message thread / workers, never the audio thread;
// reads the immutable SampleBuffer, never changes it (SLC-010). No allocation.
//
// nearestZeroCrossing: the frame f in [frame − window, frame + window] nearest to `frame` where the signal changes sign
// between f − 1 and f (stereo: the L + R sum; "negative" = sign bit set below zero, so 0 counts as positive); ties go
// to the earlier frame. `frame` itself (clamped to the sample) when the window holds no crossing.
#pragma once

#include "core/audio/SampleBuffer.h"

#include <cstdint>

namespace services::analysis
{
using core::audio::SampleBuffer;

inline constexpr double kZeroCrossingWindowMs = 5.0; // SLC-006: ±5 ms

std::int64_t nearestZeroCrossing(const SampleBuffer&, std::int64_t frame, int windowFrames) noexcept;
// The scalar reference (the NEON path must give the same frame) and, where it exists, the NEON path itself.
std::int64_t nearestZeroCrossingScalar(const SampleBuffer&, std::int64_t frame, int windowFrames) noexcept;
#if defined(__ARM_NEON)
std::int64_t nearestZeroCrossingNeon(const SampleBuffer&, std::int64_t frame, int windowFrames) noexcept;
#endif
// Window in frames for `ms` at the buffer's rate.
int windowFrames(const SampleBuffer&, double ms = kZeroCrossingWindowMs) noexcept;
} // namespace services::analysis
