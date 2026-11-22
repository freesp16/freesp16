#include "tests/fakes/FakeAudioDevice.h"

#include "core/util/Realtime.h"

#include <cstring>

namespace fakes
{
bool FakeAudioDevice::start(const platform::AudioConfig& config, engine::AudioProcessor& processor, std::string& info)
{
    config_ = config;
    const auto n = static_cast<std::size_t>(config.periodFrames);
    buffers_.assign(4 * n, 0.0f); // Main L/R + Phones L/R
    out_.fill(nullptr);
    out_[0] = buffers_.data();
    out_[1] = buffers_.data() + n;
    out_[2 * static_cast<int>(platform::AudioPort::Phones)] = buffers_.data() + 2 * n;
    out_[2 * static_cast<int>(platform::AudioPort::Phones) + 1] = buffers_.data() + 3 * n;
    processor.prepare(static_cast<double>(config.sampleRate), static_cast<int>(config.periodFrames));
    processor_ = &processor;
    info = "fake-offline: driven by the test";
    return true;
}

platform::AudioStats FakeAudioDevice::stats() const noexcept
{
    return {periods_, 0, config_.sampleRate, config_.periodFrames, false};
}

bool FakeAudioDevice::runPeriods(int periods, int frames)
{
    if (processor_ == nullptr)
        return false;
    if (frames <= 0 || frames > static_cast<int>(config_.periodFrames))
        frames = static_cast<int>(config_.periodFrames);
    capturedL_.reserve(capturedL_.size() + static_cast<std::size_t>(periods * frames));
    capturedR_.reserve(capturedR_.size() + static_cast<std::size_t>(periods * frames));
    for (int p = 0; p < periods; ++p)
    {
        {
            core::rt::ScopedRealtimeThread realtime; // the processor runs under the real-time contract
            processor_->process({out_.data(), static_cast<int>(platform::AudioPort::Count), nullptr, 0, frames,
                                 periods_ * 1000000ull});
        }
        capturedL_.insert(capturedL_.end(), out_[0], out_[0] + frames);
        capturedR_.insert(capturedR_.end(), out_[1], out_[1] + frames);
        ++periods_;
    }
    return true;
}
} // namespace fakes
