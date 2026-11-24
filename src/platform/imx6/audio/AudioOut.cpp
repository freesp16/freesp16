#include "platform/imx6/audio/AudioOut.h"

#include "core/util/Realtime.h"
#include "core/util/ThreadRole.h"

#include <cstring>

namespace platform::imx6
{
bool AudioOut::start(const AudioConfig& config, engine::AudioProcessor& processor, std::string& info)
{
    config_ = config;
    processor_ = &processor;
    if (! manager_)
        manager_ = std::make_unique<juce::AudioDeviceManager>();
    auto& manager = *manager_;
    // Device types are created lazily: getAvailableDeviceTypes() scans them, setCurrentAudioDeviceType() does not.
    juce::StringArray typeNames;
    for (auto* t : manager.getAvailableDeviceTypes())
        typeNames.add(t->getTypeName());
    manager.setCurrentAudioDeviceType("ALSA", true);
    auto* type = manager.getCurrentDeviceTypeObject();
    if (type == nullptr || type->getTypeName() != "ALSA")
    {
        info = "audio: no ALSA device type (available: " + typeNames.joinIntoString(", ").toStdString() + ")";
        return false;
    }
    type->scanForDevices();
    const auto names = type->getDeviceNames(false);
    juce::String chosen;
    for (const auto& n : names)
        if (n.containsIgnoreCase(device_.deviceHint))
        {
            chosen = n;
            break;
        }
    if (chosen.isEmpty() && ! names.isEmpty())
        chosen = names[0];
    info = "audio: ALSA outputs [" + names.joinIntoString(" | ").toStdString() + "] -> '" + chosen.toStdString() + "'";

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.outputDeviceName = chosen;
    setup.inputDeviceName = {};
    setup.sampleRate = config.sampleRate;
    setup.bufferSize = static_cast<int>(config.periodFrames); // the engine serves any size (64-frame block adapter)
    setup.useDefaultOutputChannels = false;
    setup.useDefaultInputChannels = false;
    setup.outputChannels.setRange(0, device_.channels, true);
    setup.inputChannels.clear();
    const auto error = manager.initialise(0, device_.channels, nullptr, false, {}, &setup);
    if (error.isNotEmpty())
    {
        info += " initialise: " + error.toStdString();
        return false;
    }
    const auto applied = manager.setAudioDeviceSetup(setup, true);
    if (applied.isNotEmpty())
        info += " setup: " + applied.toStdString();
    auto* device = manager.getCurrentAudioDevice();
    if (device == nullptr)
    {
        info += " (no device opened)";
        return false;
    }
    manager.addAudioCallback(this);
    started_ = true;
    info += " opened " + device->getName().toStdString() + " " + std::to_string(static_cast<int>(device->getCurrentSampleRate()))
          + " Hz, " + std::to_string(device->getCurrentBufferSizeSamples()) + " frames, "
          + std::to_string(device->getActiveOutputChannels().countNumberOfSetBits()) + " out channels";
    return true;
}

void AudioOut::stop()
{
    if (! manager_)
        return;
    if (started_)
        manager_->removeAudioCallback(this);
    manager_->closeAudioDevice();
    started_ = false;
}

AudioStats AudioOut::stats() const noexcept
{
    // JUCE's ALSA device counts its own xruns (getXRunCount(), -1 = unknown); read on the message thread.
    std::uint64_t xruns = 0;
    if (manager_ != nullptr)
        if (auto* device = manager_->getCurrentAudioDevice())
            xruns = static_cast<std::uint64_t>(juce::jmax(0, device->getXRunCount()));
    return {callbacks_.load(std::memory_order_relaxed), xruns, rate_.load(), period_.load(),
            realtime_.load(std::memory_order_relaxed)};
}

void AudioOut::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    // Message thread: size every buffer the callback needs (no allocation in the callback).
    maxFrames_ = juce::jmax(device->getCurrentBufferSizeSamples(), 4096);
    scratch_.assign(2 * static_cast<std::size_t>(maxFrames_), 0.0f);
    rate_ = static_cast<unsigned>(device->getCurrentSampleRate());
    period_ = static_cast<unsigned>(device->getCurrentBufferSizeSamples());
    processor_->prepare(device->getCurrentSampleRate(), maxFrames_);
}

void AudioOut::audioDeviceIOCallbackWithContext(const float* const*, int, float* const* out, int numOut, int numSamples,
                                                const juce::AudioIODeviceCallbackContext&)
{
    // JUCE starts its ALSA thread at Priority::high, which is SCHED_OTHER on Linux: the first callback on each thread
    // takes the audio role (its core and SCHED_FIFO, best effort), before the thread is marked real-time.
    static thread_local bool role = (realtime_.store(core::threads::apply(core::threads::Role::Audio, "audio").realtime,
                                                     std::memory_order_relaxed),
                                     true);
    (void) role;
    core::rt::ScopedRealtimeThread realtime;
    static thread_local bool ftz = (core::rt::enableFlushToZero(), true);
    (void) ftz;
    for (int ch = 0; ch < numOut; ++ch)
        if (out[ch] != nullptr)
            std::memset(out[ch], 0, sizeof(float) * static_cast<std::size_t>(numSamples));
    const int l = numOut > ports_[0].right ? ports_[0].left : 0;
    const int r = numOut > ports_[0].right ? ports_[0].right : (numOut > 1 ? 1 : 0);
    const bool direct = numOut > 1 && l != r && out[l] != nullptr && out[r] != nullptr;
    if (! direct) // the scratch pair holds maxFrames_; the direct path takes any size
        numSamples = juce::jmin(numSamples, maxFrames_);
    out_.fill(nullptr);
    out_[0] = direct ? out[l] : scratch_.data();
    out_[1] = direct ? out[r] : scratch_.data() + maxFrames_;
    processor_->process({out_.data(), static_cast<int>(AudioPort::Count), nullptr, 0, numSamples, 0});
    if (! direct) // fewer channels than the map (or a mono device): copy MAIN to what exists
    {
        if (numOut > 0 && out[l] != nullptr)
            std::memcpy(out[l], out_[0], sizeof(float) * static_cast<std::size_t>(numSamples));
        if (r != l && r < numOut && out[r] != nullptr)
            std::memcpy(out[r], out_[1], sizeof(float) * static_cast<std::size_t>(numSamples));
    }
    callbacks_.fetch_add(1, std::memory_order_relaxed);
}
} // namespace platform::imx6
