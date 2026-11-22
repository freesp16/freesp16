// The host platform: macOS/Linux, used by the host app, the tests and the host preview. Offline audio, an
// offscreen display, no lamps, and the keyboard source (UI-120, platform/host/input/KeyboardSource: a host window
// attaches it as a juce::KeyListener).
// Paths follow the XDG layout under $HOME; <PREFIX>_DATA_DIR overrides the data folder (tests).
#include "platform/Platform.h"

#include "platform/host/HostPaths.h"
#include "platform/host/audio/OfflineAudioDevice.h"
#include "platform/host/display/OffscreenDisplay.h"
#include "platform/host/input/KeyboardSource.h"

#include <string>

namespace platform::host
{
namespace
{
class HostPlatform final : public Platform
{
public:
    std::string_view id() const noexcept override { return "host"; }
    std::string describe() const override
    {
        return "host (offline audio, offscreen " + std::to_string(800) + "x" + std::to_string(480) + " display, no lamps)";
    }
    bool detect() const override { return true; }

    Paths paths() const override { return xdgPaths(); }

    std::vector<std::unique_ptr<AudioDevice>> audioDevices() override
    {
        std::vector<std::unique_ptr<AudioDevice>> devices;
        devices.push_back(std::make_unique<OfflineAudioDevice>());
        return devices;
    }

    std::vector<std::unique_ptr<io::ControlSource>> controlSources(const ControlEnv& env) override
    {
        std::vector<std::unique_ptr<io::ControlSource>> sources;
        sources.push_back(std::make_unique<KeyboardSource>(env.triggers)); // UI-120; a window attaches it
        return sources;
    }
    std::unique_ptr<io::LampSink> lamps() override { return nullptr; }
    std::unique_ptr<DisplayDevice> display(std::string&) override { return makeOffscreenDisplay(); }
};

std::unique_ptr<Platform> makePlatform() { return std::make_unique<HostPlatform>(); }
} // namespace
} // namespace platform::host

APP_REGISTER_PLATFORM("host", &platform::host::makePlatform)
