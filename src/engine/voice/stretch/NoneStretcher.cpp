// "none": no time-stretch engine (audio.stretchBackend = none). It only
// resamples — the length follows the time ratio and the pitch follows it too (pitchScale is ignored), which is what
// MASTER TEMPO degrades to without an engine: RESAMPLE. Cubic Hermite, Q32.32 position, exact framesNeeded.
#include "engine/voice/TimeStretcher.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace engine::voice::stretch
{
namespace
{
class NoneStretcher final : public TimeStretcher
{
public:
    void prepare(double, int maxBlock, int channels) override
    {
        channels_ = std::clamp(channels, 1, 2);
        capacity_ = 4 * std::max(1, maxBlock) + 16; // time ratio ≥ 0.25
        for (auto& s : stage_)
            s.assign(static_cast<std::size_t>(capacity_ + 8), 0.0f);
        reset();
    }
    void reset() noexcept override
    {
        base_ = -2; // two zero frames before input frame 0
        written_ = 0;
        pos_ = 0;
        for (int c = 0; c < channels_; ++c)
            stage_[c][0] = stage_[c][1] = 0.0f;
    }
    void setRatios(double timeRatio, double) noexcept override
    {
        const double t = std::isfinite(timeRatio) ? std::clamp(timeRatio, 0.25, 4.0) : 1.0;
        step_ = static_cast<std::int64_t>(4294967296.0 / t);
    }
    int latencySamples() const noexcept override { return 0; }
    int framesNeeded(int outFrames) const noexcept override
    {
        if (outFrames <= 0)
            return 0;
        const std::int64_t last = (pos_ + (outFrames - 1) * step_) >> 32;
        const std::int64_t need = last + 3 - written_;
        return need <= 0 ? 0 : static_cast<int>(std::min<std::int64_t>(need, capacity_));
    }
    int process(const float* const* in, int inFrames, float* const* out, int outFrames) noexcept override
    {
        const std::int64_t room = static_cast<std::int64_t>(stage_[0].size()) - (written_ - base_);
        const std::int64_t n = std::clamp<std::int64_t>(inFrames, 0, room);
        for (int c = 0; c < channels_; ++c)
            if (n > 0)
                std::memcpy(stage_[c].data() + (written_ - base_), in[c], static_cast<std::size_t>(n) * sizeof(float));
        written_ += n;
        int j = 0;
        for (; j < outFrames; ++j)
        {
            const std::int64_t i = pos_ >> 32;
            if (i + 2 >= written_)
                break;
            const float f = static_cast<float>(static_cast<double>(pos_ & 0xffffffffll) * (1.0 / 4294967296.0));
            for (int c = 0; c < channels_; ++c)
            {
                const float* x = stage_[c].data() + (i - base_);
                const float xm1 = x[-1], x0 = x[0], x1 = x[1], x2 = x[2];
                const float c1 = 0.5f * (x1 - xm1);
                const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
                const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
                out[c][j] = ((c3 * f + c2) * f + c1) * f + x0;
            }
            pos_ += step_;
        }
        for (int c = 0; c < channels_; ++c)
            std::fill(out[c] + j, out[c] + outFrames, 0.0f);
        const std::int64_t keep = std::min((pos_ >> 32) - 1, written_);
        if (keep > base_)
        {
            const std::int64_t shift = keep - base_, left = written_ - keep;
            const auto bytes = static_cast<std::size_t>(left > 0 ? left : 0) * sizeof(float);
            for (int c = 0; c < channels_; ++c)
                if (bytes > 0)
                    std::memmove(stage_[c].data(), stage_[c].data() + shift, bytes);
            base_ = keep;
        }
        return outFrames;
    }

private:
    int channels_ = 2, capacity_ = 0;
    std::vector<float> stage_[2];
    std::int64_t base_ = -2, written_ = 0, pos_ = 0, step_ = std::int64_t {1} << 32;
};

std::unique_ptr<TimeStretcher> makeNone()
{
    return std::make_unique<NoneStretcher>(); // rt-ok: factory, message thread (StretcherBank::build)
}
} // namespace
} // namespace engine::voice::stretch

APP_REGISTER_STRETCHER("none", &engine::voice::stretch::makeNone)
