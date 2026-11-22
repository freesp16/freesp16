// REC THRESHOLD detection (spec 02-features/sampling.md SMP-002): the first frame of a block at which
// the input exceeds the threshold, on either channel. Threshold index (sampling.threshold): 0 = OFF, 1…41 = −60…−20 dB
// in 1 dB steps, 42 = PLAY (the sequencer start edge). The level is the sample's absolute value ("exceeds": strictly
// greater), the sampler's source after the soft limiter.
//
// Kernels: scalar reference + NEON (4 lanes compare, first set lane), bit-identical by construction (only
// compares). Budget ≤ 60 A9 cycles per 64-frame stereo block in the common case (no crossing: one max pass).
#pragma once

#include <cstdint>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
 #define APP_SAMPLING_HAVE_NEON 1
#else
 #define APP_SAMPLING_HAVE_NEON 0
#endif

namespace engine::sampling
{
constexpr int kThresholdOff = 0, kThresholdPlay = 42;
constexpr int kThresholdMinDb = -60, kThresholdMaxDb = -20;

// Linear amplitude of threshold index 1…41 (−60…−20 dB); 0 for OFF / PLAY.
float thresholdGain(int index) noexcept;
// The threshold in dB of index 1…41 (tests, UI); 0 otherwise.
constexpr int thresholdDb(int index) noexcept { return index >= 1 && index <= 41 ? index - 61 : 0; }

namespace scalar
{
// First i in [0, n) with |l[i]| > thr or |r[i]| > thr (r may equal l); −1 when none.
int firstAbove(const float* l, const float* r, int n, float thr) noexcept;
} // namespace scalar
#if APP_SAMPLING_HAVE_NEON
namespace neon
{
int firstAbove(const float* l, const float* r, int n, float thr) noexcept;
} // namespace neon
#endif

inline int firstAbove(const float* l, const float* r, int n, float thr) noexcept
{
#if APP_SAMPLING_HAVE_NEON
    return neon::firstAbove(l, r, n, thr);
#else
    return scalar::firstAbove(l, r, n, thr);
#endif
}
} // namespace engine::sampling
