// The host platform's audio device ("offline"): a paced thread that drives the engine at the period rate
// without a sound card, so the host app and the tests run the real-time path (ScopedRealtimeThread, FTZ, any frame
// count) anywhere. The output is discarded; stats() counts periods. It never misses a deadline, so xruns stay 0; a
// wake-up more than one period late (an overloaded host) re-anchors the schedule and is counted in latePeriods().
// A JUCE default-device variant for the host preview needs juce_audio_devices in the host build.
#pragma once

#include "platform/Platform.h"

#include <array>
#include <atomic>
#include <thread>
#include <vector>

namespace platform::host
{
class OfflineAudioDevice final : public AudioDevice
{
public:
    ~OfflineAudioDevice() override { stop(); }
    std::string_view id() const noexcept override { return "offline"; }
    std::span<const PortInfo> ports() const noexcept override { return ports_; }
    bool start(const AudioConfig&, engine::AudioProcessor&, std::string& info) override;
    void stop() override;
    AudioStats stats() const noexcept override;
    std::uint64_t latePeriods() const noexcept { return late_.load(std::memory_order_relaxed); }

private:
    void run();

    std::array<PortInfo, 1> ports_ {{{AudioPort::Main, 0, 1}}};
    AudioConfig config_ {};
    engine::AudioProcessor* processor_ = nullptr;
    std::vector<float> buffer_;
    std::array<float*, 2 * static_cast<int>(AudioPort::Count)> out_ {};
    std::thread thread_;
    std::atomic<bool> quit_ {false};
    std::atomic<std::uint64_t> periods_ {0}, late_ {0};
};
} // namespace platform::host
