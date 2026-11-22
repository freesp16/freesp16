// Interpolation kernels of the sample voices (PLY-021).
//
// Positions are Q32.32 fixed point (frames << 32) in a signed 64-bit integer: the integer part (the tap index)
// advances exactly in 64 bits, the fraction is the low 32 bits — exact and drift-free over any sample length below
// 2^31 frames, and the same in the scalar reference and the NEON version (the NEON kernel advances the fractions as
// four uint32 lanes, which wrap exactly like the low word). Negative steps play backwards (reverse).
//
//   hermite4   4-point, 3rd-order Hermite (Catmull-Rom) on taps idx−1 … idx+2. Unchecked: the caller guarantees the
//              taps are readable (SampleBuffer::pad). NEON: weights of 4 frames at once (Horner, structure of
//              arrays), one unaligned vld1q per frame and channel, a 4 × 4 transpose, 4 multiply-adds.
//   copy       the fast path rate == ±1, fraction 0 (unpitched one-shots): a plain (or reversed) copy
//   linear     2-point linear (reference for tests)
//   sinc16     16-tap Kaiser-windowed sinc (render / quality mode, PLY-021 P2): 256 phases, linear between phases,
//              cutoff lowered with the rate (5 tables up to 4×) so transposing up does not alias; always checked
//   *Checked   any position: taps outside [−pad, srcFrames + pad) read as zero (generic Interpolator::render)
//
// Real-time safe: no allocation, no libm in the loops, no branch per sample in the unchecked kernels.
#pragma once

#include "engine/dsp/simd/Kernels.h" // APP_ENGINE_HAVE_NEON

#include <cstdint>

namespace engine::voice::kernels
{
using Fixed = std::int64_t; // Q32.32 frames
constexpr int kFracBits = 32;
constexpr Fixed kOne = Fixed {1} << kFracBits;

inline Fixed toFixed(double frames) noexcept
{
    const double v = frames * 4294967296.0;
    return static_cast<Fixed>(v < 0.0 ? v - 0.5 : v + 0.5);
}
inline double fromFixed(Fixed p) noexcept { return static_cast<double>(p) * (1.0 / 4294967296.0); }
inline std::int64_t indexOf(Fixed p) noexcept { return p >> kFracBits; } // floor
inline float fractionOf(Fixed p) noexcept
{
    return static_cast<float>(static_cast<std::uint32_t>(p)) * (1.0f / 4294967296.0f);
}

// Hermite weights of the taps idx−1, idx, idx+1, idx+2 at fraction x (they sum to 1).
struct HermiteWeights
{
    float wm1, w0, w1, w2;
};
inline HermiteWeights hermiteWeights(float x) noexcept
{
    const float x2 = x * x;
    return {((-0.5f * x + 1.0f) * x - 0.5f) * x, (1.5f * x - 2.5f) * x2 + 1.0f, ((-1.5f * x + 2.0f) * x + 0.5f) * x,
            (0.5f * x - 0.5f) * x2};
}

namespace scalar
{
void hermite4(const float* const* src, int channels, Fixed pos, Fixed step, float* const* dst, int frames) noexcept;
void linear(const float* const* src, int channels, Fixed pos, Fixed step, float* const* dst, int frames) noexcept;
} // namespace scalar

#if APP_ENGINE_HAVE_NEON
namespace neon
{
void hermite4(const float* const* src, int channels, Fixed pos, Fixed step, float* const* dst, int frames) noexcept;
} // namespace neon
#endif

inline void hermite4(const float* const* src, int channels, Fixed pos, Fixed step, float* const* dst,
                     int frames) noexcept
{
#if APP_ENGINE_HAVE_NEON
    neon::hermite4(src, channels, pos, step, dst, frames);
#else
    scalar::hermite4(src, channels, pos, step, dst, frames);
#endif
}

// dst[c][k] = src[c][index + k] (direction +1) or src[c][index − k] (direction −1).
void copy(const float* const* src, int channels, std::int64_t index, int direction, float* const* dst,
          int frames) noexcept;

// Checked variants: srcFrames frames plus `pad` readable frames on each side; taps beyond read as zero.
void hermite4Checked(const float* const* src, int channels, std::int64_t srcFrames, int pad, Fixed pos, Fixed step,
                     float* const* dst, int frames) noexcept;
void linearChecked(const float* const* src, int channels, std::int64_t srcFrames, int pad, Fixed pos, Fixed step,
                   float* const* dst, int frames) noexcept;
void sinc16Checked(const float* const* src, int channels, std::int64_t srcFrames, int pad, Fixed pos, Fixed step,
                   float* const* dst, int frames) noexcept;
constexpr int kSincTaps = 16; // taps idx − 7 … idx + 8
} // namespace engine::voice::kernels
