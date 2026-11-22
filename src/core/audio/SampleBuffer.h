// A decoded sample in RAM (audio-engine.md "Sample memory"). The implementation of retain/release is in
// core/audio/SampleBuffer.cpp.
#pragma once

#include <atomic>
#include <cstdint>

// clang-format off
namespace core::audio {
struct SampleBuffer {
    static constexpr int pad = 4;                 // ≥ Interpolator::taps(); zero-filled before 0 and after frames
    std::int64_t frames;  std::uint8_t channels;  // 1 or 2 (more are mixed down on load)
    double sampleRate;                            // = engine rate (resampled on load)
    const float* ch[2];                           // planar, 16-byte aligned, ch[i][-pad … frames+pad) readable
    std::uint64_t contentId;                      // content hash (pool key)
    mutable std::atomic<std::uint32_t> refs;      // audio thread only decrements; 0 → RetireQueue
};
void retain(const SampleBuffer*) noexcept;  void release(const SampleBuffer*) noexcept;  // RT-safe
}
// clang-format on
