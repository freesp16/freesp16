// "wsola": the own time-domain backend (a cheap time-domain fallback). WSOLA on top
// of GrainStretcher: Hann grains of W = 1024 frames (2048 above 64 kHz) at hop W/2; each grain's analysis start moves
// by δ ∈ [−Δ, Δ] to where its first half best matches the natural continuation of the previous grain (normalised
// cross-correlation of the mid signal L + R; the same δ for both channels, so the stereo image is kept). The search is
// coarse on a 2× decimated mid signal (step 4 frames), then refined at full resolution around the best candidate.
//   tier 0   Δ = W/4 (≈ 5.8 ms at 44.1 kHz: periods down to ≈ 86 Hz align), coarse step 4
//   tier 1   Δ = W/8, coarse step 8 ("cheaper")
// Cost per grain: about (2Δ/step)·W/4 + (2·step)·W/2 multiply-adds in kernels::dot (NEON), one grain per W/2 output
// frames; transients stay sharp (no spectral smearing), sustained harmonic sounds can show a faint grain rate.
#pragma once

#include "engine/voice/stretch/GrainStretcher.h"

#include <memory>

namespace engine::voice::stretch
{
class WsolaStretcher final : public GrainStretcher
{
public:
    static std::unique_ptr<TimeStretcher> make();

protected:
    Geometry geometryFor(int tier, double sampleRate) const noexcept override;
    void allocateCore(int channels) override;
    void resetCore() noexcept override;
    void synthesise(std::int64_t anaStart, int anaHop) noexcept override;

private:
    int coarseStep() const noexcept { return quality() == 0 ? 4 : 8; }
    void readMid(std::int64_t start, int n, float* mid) noexcept;

    std::vector<float> win_, frame_, tmp_;
    std::vector<float> region_, tmpl_, regionD_, tmplD_;
    std::vector<double> energy_, energyD_;
    std::int64_t prevActual_ = 0;
};
} // namespace engine::voice::stretch
