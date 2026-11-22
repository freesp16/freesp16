// "pvoc": the own phase-vocoder backend — the spectral candidate, in the role of Signalsmith
// Stretch / Rubber Band R2 (no third-party code; THIRD_PARTY.md). On top of GrainStretcher:
//
//   grain   Hann analysis window W, zero-padded FFT N = 1024 at 44.1/48 kHz (2048 above 64 kHz); tier 0 hop W/4,
//           tier 1 ("cheaper") W = 1020, hop 340 (overlap 3: Hann² still sums to a constant)
//   stereo  one complex FFT carries both channels (z = L + iR); the phase propagation runs once, on the mid spectrum
//           M = L + R, and the same per-bin rotation e^(iθ) is applied to L and R, so the inter-channel phase (the
//           stereo image) and a mono-compatible sum are kept exactly
//   phase   identity phase locking (Laroche–Dolson): magnitude peaks get the instantaneous-frequency phase advance
//           ψ += ω_inst·Hs; every other bin takes the rotation of the peak whose region (bounded by the lowest bin
//           between two peaks) it lies in. Integer-exact expected advances ((k·hop) mod N), so long notes do not
//           drift; θ is kept wrapped to [−π, π]. The W/hop grains after reset() overlapping output frame 0 keep the
//           analysis phases (phase reset at the note start: a clean onset, no pre-echo) and skip the FFT round trip
//           (the grain is its own windowed input), which keeps the priming burst to one forward FFT
//
// Per grain: 2 complex FFTs (forward, inverse), atan2 / |M|² / rotation over N/2 bins (NEON kernels), one sin/cos per
// peak. Cost per output frame does not depend on the time ratio (one grain per synthesis hop).
#pragma once

#include "engine/voice/stretch/GrainStretcher.h"
#include "engine/voice/stretch/StretchFft.h"

#include <memory>

namespace engine::voice::stretch
{
class PvocStretcher final : public GrainStretcher
{
public:
    static std::unique_ptr<TimeStretcher> make();

protected:
    Geometry geometryFor(int tier, double sampleRate) const noexcept override;
    void allocateCore(int channels) override;
    void resetCore() noexcept override;
    void synthesise(std::int64_t anaStart, int anaHop) noexcept override;

private:
    StretchFft fft_;
    int n_ = 1024, bins_ = 513;
    std::vector<float> anaWin_, synWin_, identWin_; // [tier][N]
    std::vector<float> re_, im_, frame_[2];
    std::vector<float> xr_[2], xi_[2];          // channel spectra, bins 0…N/2
    std::vector<float> mr_, mi_, mag_, phase_;  // mid spectrum
    std::vector<float> prevPhase_, prevTheta_, theta_, rotC_, rotS_;
    std::vector<int> peaks_;
};
} // namespace engine::voice::stretch
