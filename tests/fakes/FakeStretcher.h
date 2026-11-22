// FakeStretcher: an engine::voice::TimeStretcher that only resamples (linear interpolation) — the length follows the
// time ratio and the pitch follows it too (no pitch preservation). Enough to test MASTER TEMPO plumbing, latency and
// the stretcher contract harness without a real backend.
#pragma once

#include "engine/voice/TimeStretcher.h"

#include <cmath>

namespace fakes
{
class FakeStretcher final : public engine::voice::TimeStretcher
{
public:
    void prepare(double, int, int channels) override { channels_ = channels; }
    void reset() noexcept override { phase_ = 0; }
    void setRatios(double timeRatio, double) noexcept override { step_ = timeRatio > 0 ? 1.0 / timeRatio : 1.0; }
    int latencySamples() const noexcept override { return 0; }
    int framesNeeded(int outFrames) const noexcept override
    {
        return static_cast<int>(std::floor(phase_ + step_ * outFrames)) + 1;
    }
    int process(const float* const* in, int inFrames, float* const* out, int outFrames) noexcept override
    {
        int written = 0;
        for (; written < outFrames; ++written)
        {
            const auto i = static_cast<int>(phase_);
            if (i + 1 >= inFrames)
                break;
            const auto f = static_cast<float>(phase_ - i);
            for (int c = 0; c < channels_; ++c)
                out[c][written] = in[c][i] + (in[c][i + 1] - in[c][i]) * f;
            phase_ += step_;
        }
        phase_ -= std::floor(phase_); // keep the fractional position; the caller advances its input by framesNeeded - 1
        return written;
    }

private:
    int channels_ = 1;
    double step_ = 1.0, phase_ = 0.0;
};
} // namespace fakes
