#include "platform/host/audio/OfflineAudioDevice.h"

#include "core/util/Realtime.h"

#include <chrono>
#include <string>

namespace platform::host
{
bool OfflineAudioDevice::start(const AudioConfig& config, engine::AudioProcessor& processor, std::string& info)
{
    stop();
    config_ = config;
    processor_ = &processor;
    const auto frames = static_cast<std::size_t>(config.periodFrames);
    buffer_.assign(2 * frames, 0.0f);
    out_.fill(nullptr);
    out_[0] = buffer_.data();
    out_[1] = buffer_.data() + frames;
    processor.prepare(static_cast<double>(config.sampleRate), static_cast<int>(config.periodFrames));
    periods_ = 0;
    late_ = 0;
    quit_ = false;
    thread_ = std::thread([this] { run(); });
    info = "offline: " + std::to_string(config.sampleRate) + " Hz, period " + std::to_string(config.periodFrames)
         + " frames, paced, output discarded";
    return true;
}

void OfflineAudioDevice::stop()
{
    quit_ = true;
    if (thread_.joinable())
        thread_.join();
}

AudioStats OfflineAudioDevice::stats() const noexcept
{
    return {periods_.load(std::memory_order_relaxed), 0, config_.sampleRate, config_.periodFrames, false};
}

void OfflineAudioDevice::run()
{
    core::rt::enableFlushToZero();
    core::rt::ScopedRealtimeThread realtime;
    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::duration<double>(static_cast<double>(config_.periodFrames) / config_.sampleRate);
    auto next = clock::now();
    const engine::AudioIO io {out_.data(), static_cast<int>(AudioPort::Count), nullptr, 0,
                              static_cast<int>(config_.periodFrames), 0};
    while (! quit_.load(std::memory_order_relaxed))
    {
        engine::AudioIO now = io;
        now.hostTimeNs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count());
        processor_->process(now);
        periods_.fetch_add(1, std::memory_order_relaxed);
        next += std::chrono::duration_cast<clock::duration>(period);
        const auto now2 = clock::now();
        if (now2 > next + std::chrono::duration_cast<clock::duration>(period))
        {
            // More than one period late (an overloaded host, a debugger stop): re-anchor the schedule instead of
            // bursting to catch up. Not an xrun — there is no device to run dry (stats().xruns stays 0).
            late_.fetch_add(1, std::memory_order_relaxed);
            next = now2;
        }
        std::this_thread::sleep_until(next); // not real-time critical: there is no device to miss
    }
}
} // namespace platform::host
