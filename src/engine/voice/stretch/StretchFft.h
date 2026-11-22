// In-place complex FFT (radix 2, decimation in time, structure of arrays) for the phase-vocoder backend.
// prepare() allocates the bit-reversal table and one contiguous twiddle table per stage; forward() and inverse() are
// real-time safe. The stages with half-size ≥ 4 run kernels::butterflies (NEON on the device).
// inverse() is unscaled (the caller folds 1/N into its synthesis window): inverse(x) = N · IDFT(x).
#pragma once

#include <vector>

namespace engine::voice::stretch
{
class StretchFft
{
public:
    void prepare(int size); // size: a power of two ≥ 4 (message thread)
    int size() const noexcept { return n_; }
    void forward(float* re, float* im) const noexcept;
    void inverse(float* re, float* im) const noexcept;

private:
    void permute(float* re, float* im) const noexcept;
    int n_ = 0;
    std::vector<int> swaps_;        // pairs (i, j), i < j, of the bit-reversal permutation
    std::vector<float> wr_, wi_;    // stage h (h = 1, 2, 4, …): entries [h − 1, 2h − 1)
};
} // namespace engine::voice::stretch
