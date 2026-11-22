// Windowed-sinc resampler for loading samples at the engine rate (LIB-020):
// Kaiser-windowed sinc, polyphase, linear phase (no delay), float32, one channel at a time, on a worker.
//
//   taps   kBaseTaps at unity ratio; when downsampling by r the kernel widens to ⌈kBaseTaps / r⌉
//          (rounded to a multiple of 4) so the transition band stays the same in output terms; ≤ 256
//   cutoff kCutoff × min(input, output) Nyquist
//   phases exact for rational rate pairs (both rates whole numbers, reduced output factor ≤ 4096: every rate pair of
//          8–192 kHz to 44.1/48 kHz); otherwise 4096 phases with linear interpolation between neighbouring phases
//
// 40 taps / Kaiser β = 10 instead of 32 / β = 8: measured on 48 → 44.1 kHz sines every 611 Hz in 0–18 kHz,
// 32 / β 8 leaves a worst residual of −90.3 dB (no margin on the ≤ −90 dB acceptance), 40 / β 10
// −106 dB (48 / β 10: −107 dB) for 25 % more multiply-adds.
//
// The dot product has a scalar reference and a NEON kernel (ARMv7 intrinsics subset, __ARM_NEON), tested against
// each other; budget ≥ 15× real time for stereo 48 → 44.1 kHz on one A9 core (bench_decode `decode_resample`).
#pragma once

#include <cstdint>
#include <vector>

namespace services::library
{
class Resampler
{
public:
    static constexpr int kBaseTaps = 40;
    static constexpr double kBeta = 10.0;
    static constexpr double kCutoff = 0.96;
    static constexpr int kMaxPhases = 4096;

    Resampler(double inputRate, double outputRate, int baseTaps = kBaseTaps, double beta = kBeta,
              double cutoff = kCutoff);

    double inputRate() const noexcept { return in_; }
    double outputRate() const noexcept { return out_; }
    int taps() const noexcept { return taps_; }
    int phases() const noexcept { return phases_; }
    bool exact() const noexcept { return exact_; }

    // ⌈inFrames × out / in⌉.
    std::int64_t outputFrames(std::int64_t inFrames) const noexcept;

    // Resamples in[0, inFrames) (zero outside) into out[0, outFrames). Output n sits at input time n × in / out.
    enum class Kernel
    {
        Auto,
        Scalar,
        Neon
    };
    void process(const float* in, std::int64_t inFrames, float* out, std::int64_t outFrames,
                 Kernel kernel = Kernel::Auto) const noexcept;

    static bool neonAvailable() noexcept;

private:
    const float* phase(int k) const noexcept { return table_.data() + static_cast<std::size_t>(k) * taps_; }

    double in_, out_;
    int taps_ = 0, phases_ = 0;
    bool exact_ = false;
    std::int64_t step_ = 0;    // exact: input advance per output, in 1/phases_ input frames
    double ratio_ = 1;         // inexact: input frames per output
    std::vector<float> table_; // (phases_ + 1) × taps_, 16-byte aligned rows (taps_ % 4 == 0)
};
} // namespace services::library
