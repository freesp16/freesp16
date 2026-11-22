// Pitch playback and sample-rate conversion kernels (PLY-*, audio-engine.md
// "Sample memory"): `hermite4` (default, NEON) and `sinc16` (render/quality mode), chosen per voice. Called once per
// block per voice (no virtual call per sample); the inner loop is the implementation's.
#pragma once

#include <cstdint>

namespace engine::voice
{
class Interpolator {                                  // pitch playback + sample-rate conversion
public:
    virtual ~Interpolator() = default;
    // Reads `frames` output frames from src starting at position `pos` (frames, fractional) with increment `rate`,
    // mono or stereo planar; returns the new position. No allocation; vectorisable inner loop.
    virtual double render(const float* const* src, int channels, std::int64_t srcFrames, double pos, double rate,
                          float* const* dst, int frames) noexcept = 0;
    virtual int taps() const noexcept = 0;            // for loop/edge padding
};
} // namespace engine::voice
