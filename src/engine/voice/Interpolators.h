// The Interpolator implementations (contract engine/voice/Interpolator.h; PLY-021):
//   Hermite4  "hermite4", the default: 4-point Hermite, NEON with a scalar reference (InterpKernels)
//   Sinc16    "sinc16", render / quality mode: 16-tap Kaiser-windowed sinc, cutoff follows the rate
//   Linear    "linear", reference for tests
// render() accepts any source buffer: taps outside [0, srcFrames) read as zero, so no padding is required (a padded
// SampleBuffer gives the same result). The voices call the unchecked kernels directly on padded buffers instead.
#pragma once

#include "engine/voice/Interpolator.h"

#include <string_view>

namespace engine::voice
{
enum class InterpolatorKind : std::uint8_t
{
    Hermite4,
    Sinc16,
    Linear
};

std::string_view interpolatorName(InterpolatorKind) noexcept;

class Hermite4 final : public Interpolator
{
public:
    double render(const float* const* src, int channels, std::int64_t srcFrames, double pos, double rate,
                  float* const* dst, int frames) noexcept override;
    int taps() const noexcept override { return 4; }
};

class Sinc16 final : public Interpolator
{
public:
    double render(const float* const* src, int channels, std::int64_t srcFrames, double pos, double rate,
                  float* const* dst, int frames) noexcept override;
    int taps() const noexcept override { return 16; }
};

class Linear final : public Interpolator
{
public:
    double render(const float* const* src, int channels, std::int64_t srcFrames, double pos, double rate,
                  float* const* dst, int frames) noexcept override;
    int taps() const noexcept override { return 2; }
};
} // namespace engine::voice
