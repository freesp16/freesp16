// FakeAudioDevice: a platform::AudioDevice that is an offline callback driver. start() prepares the processor like a
// real device; runPeriods() then calls process() on the *calling* thread, marked real-time (ScopedRealtimeThread: any
// allocation aborts under APP_RT_CHECKS), and captures the Main pair. Deterministic: no thread, no clock.
#pragma once

#include "platform/Platform.h"

#include <array>
#include <vector>

namespace fakes
{
class FakeAudioDevice final : public platform::AudioDevice
{
public:
    std::string_view id() const noexcept override { return "fake-offline"; }
    std::span<const platform::PortInfo> ports() const noexcept override { return ports_; }
    bool start(const platform::AudioConfig&, engine::AudioProcessor&, std::string& info) override;
    void stop() override { processor_ = nullptr; }
    platform::AudioStats stats() const noexcept override;

    // Drives `periods` periods of `frames` frames (0 = the configured period; must not exceed it). Returns false when
    // the device is not started.
    bool runPeriods(int periods, int frames = 0);
    const std::vector<float>& mainLeft() const noexcept { return capturedL_; }
    const std::vector<float>& mainRight() const noexcept { return capturedR_; }
    void clearCapture() { capturedL_.clear(), capturedR_.clear(); }
    bool started() const noexcept { return processor_ != nullptr; }

private:
    std::array<platform::PortInfo, 2> ports_ {{{platform::AudioPort::Main, 0, 1}, {platform::AudioPort::Phones, 2, 3}}};
    platform::AudioConfig config_ {};
    engine::AudioProcessor* processor_ = nullptr;
    std::vector<float> buffers_;
    std::array<float*, 2 * static_cast<int>(platform::AudioPort::Count)> out_ {};
    std::vector<float> capturedL_, capturedR_;
    std::uint64_t periods_ = 0;
};
} // namespace fakes
