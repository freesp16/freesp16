// The imx6 platform's fallback audio device ("juce-alsa"): JUCE's ALSA backend at 44.1 kHz. The device callback
// is the real-time thread: it marks itself (ScopedRealtimeThread), enables flush-to-zero and calls the engine once per
// callback with the MAIN pair pointing straight into JUCE's output buffers of the device's channel map (no copy).
//
// Channel map: the master stream (hw:ak4458audio,0) is 16 S16 channels, MAIN L/R = channels 12/14
// (sp16-emulator docs/audio-jack-map.md, grade O = observed in the emulator); devices with fewer channels get 0/1.
#pragma once

#include "platform/Platform.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace platform::imx6
{
struct JuceAudioConfig
{
    juce::String deviceHint = "ak4458"; // substring of the ALSA output device name
    int channels = 16;
};

class AudioOut final : public AudioDevice, private juce::AudioIODeviceCallback
{
public:
    explicit AudioOut(JuceAudioConfig config = {}) : device_(std::move(config)) {}
    ~AudioOut() override { stop(); }

    std::string_view id() const noexcept override { return "juce-alsa"; }
    std::span<const PortInfo> ports() const noexcept override { return ports_; }
    bool start(const AudioConfig&, engine::AudioProcessor&, std::string& info) override;
    void stop() override;
    AudioStats stats() const noexcept override;

private:
    void audioDeviceIOCallbackWithContext(const float* const* in, int numIn, float* const* out, int numOut, int numSamples,
                                          const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override {}

    JuceAudioConfig device_;
    AudioConfig config_ {};
    std::array<PortInfo, 6> ports_ {{{AudioPort::Main, 12, 14},
                                     {AudioPort::Out34, -1, -1},
                                     {AudioPort::Out56, -1, -1},
                                     {AudioPort::Out78, -1, -1},
                                     {AudioPort::Phones, -1, -1},
                                     {AudioPort::FilterSend, -1, -1}}};
    engine::AudioProcessor* processor_ = nullptr;
    std::unique_ptr<juce::AudioDeviceManager> manager_;
    std::vector<float> scratch_; // MAIN pair when the device's buffers cannot be used directly
    std::array<float*, 2 * static_cast<int>(AudioPort::Count)> out_ {};
    std::atomic<std::uint64_t> callbacks_ {0};
    std::atomic<unsigned> rate_ {0}, period_ {0};
    int maxFrames_ = 0;
    bool started_ = false;
};
} // namespace platform::imx6
