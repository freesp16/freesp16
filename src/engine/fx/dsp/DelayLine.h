// Planar stereo delay ring of the effects DSP kit (the Insert/Send delays, Chorus/Flanger, Reverb).
//
// - Allocated once by prepare() on the message thread at its exact maximum (no power-of-two rounding: the 32 Insert
//   Delay rings dominate the FX memory budget); never resized on the audio thread.
// - O(1) logical clear: clear() resets a fill counter, and every read of a sample older than the samples written since
//   the clear returns 0. kClearState and the second STOP (FX-036) therefore never memset megabytes in one block.
// - Reads address "age": the sample written `age` writes ago (age 1 = the newest). Integer, linear and 4-point Hermite
//   reads; block reads with an integer delay copy at most two contiguous segments (NEON copy).
#pragma once

#include "engine/fx/dsp/Numerics.h"

#include <cstddef>
#include <memory>

namespace engine::fx::dsp
{
class DelayLine
{
public:
    // Message thread. Capacity = maxAge + 4 (room for the Hermite neighbours). Returns the bytes allocated.
    std::size_t prepare(int maxAge);
    std::size_t bytes() const noexcept { return 2 * sizeof(float) * static_cast<std::size_t>(cap_); }
    int capacity() const noexcept { return cap_; }
    int maxAge() const noexcept { return cap_ - 4; }

    void clear() noexcept { filled_ = 0; } // O(1): every sample written before reads as 0
    void hardClear() noexcept;             // memset (start-up / tests); not for the audio thread's hot path

    void write(float l, float r) noexcept
    {
        bufL_[w_] = l;
        bufR_[w_] = r;
        if (++w_ == cap_)
            w_ = 0;
        if (filled_ < cap_)
            ++filled_;
    }
    void writeBlock(const float* l, const float* r, int n) noexcept;

    // Sample written `age` writes ago (1 = newest), 0 when older than the last clear() or than the capacity.
    float readL(int age) const noexcept { return age <= filled_ && age >= 1 ? bufL_[index(age)] : 0.0f; }
    float readR(int age) const noexcept { return age <= filled_ && age >= 1 ? bufR_[index(age)] : 0.0f; }
    // Fractional age (>= 1 for linear, >= 2 for Hermite).
    void readLinear(float age, float& l, float& r) const noexcept;
    void readHermite(float age, float& l, float& r) const noexcept;
    // out[j] = the sample written (age − j) writes ago, j = 0…n−1; age >= n (call before writing this block).
    void readBlock(float* l, float* r, int age, int n) const noexcept;

private:
    int index(int age) const noexcept
    {
        const int i = w_ - age;
        return i < 0 ? i + cap_ : i;
    }
    std::unique_ptr<float[]> storeL_, storeR_;
    float* bufL_ = nullptr;
    float* bufR_ = nullptr;
    int cap_ = 0, w_ = 0, filled_ = 0;
};
} // namespace engine::fx::dsp
