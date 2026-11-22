// The Linux desktop: the full app natively on a Linux amd64 desktop — a managed X11 window of 800 × 480 ×
// `scale` (soft + gl-x11 through gldispatch's Mesa flavour), real audio (direct ALSA on `default`, through the
// PipeWire/PulseAudio plugins) or the `file` device (a WAV rendered in real time, for the automated checks), the
// keyboard (UI-120) and the mouse as the LCD touch. Built on Linux hosts only (platform/CMakeLists.txt).
//
// detect(): Linux, not the reference hardware (no /dev/subucom_spi0.1, no /proc/asound/ak4458audio), DISPLAY set.
// Forced with <PREFIX>_PLATFORM=linux / platform=linux. The host platform stays the fallback (headless runs, tests).
//
// Settings (key=value in ~/.config/<name>/<name>.conf or <PREFIX>_<KEY>):
//   audio_device=file:<path.wav>   the file device (then offline);   audio_device=<ALSA PCM>  (default `default`) the
//                                  direct ALSA device, 2 channels, MAIN = 0/1 (then offline). The app's
//                                  built-in default hw:ak4458audio,0 (a device PCM) means `default` here.
//   audio_file_ports=main|all      the file device's WAV: MAIN only (2 channels, default) or every logical port (12
//                                  channels in AudioPort order, MAIN still 0/1). The direct ALSA
//                                  device stays MAIN only: OUT3-4…7-8 are absent there (assigned tracks fold into
//                                  MAIN, MIX-032) and the headphone cue is not rendered.
//   audio_input=file:<in.wav>      the file device's LINE IN stand-in (16-bit WAV, looped; no ALSA capture here)
//   audio_channels=2..32           the direct ALSA device's channel count (default 2) and
//   audio_map=<port>:<L>/<R>,…     its port map over MAIN = 0/1 (names as imx6: main, out34, out56, out78, phones,
//                                  filtersend; app/App.cpp derives both from UTILITY › OUTPUT CHANNEL MAP, MIX-032).
//                                  When that device does not open, the stereo MAIN-only device is tried next.
//   scale=1..4                     window = 800·scale × 480·scale pixels (default 1)
//   lamp_log=0|1                   LAMP lines on change (default 0: no lamps)
// No JUCE ALSA fallback: the host JUCE library has no juce_audio_devices (cmake/Juce.cmake).
#include "platform/Platform.h"

#include "platform/common/audio/AlsaOutput.h"
#include "platform/host/HostPaths.h"
#include "platform/host/audio/OfflineAudioDevice.h"
#include "platform/common/LogLamps.h"
#include "platform/linux/audio/FileAudioDevice.h"
#include "platform/linux/display/LinuxDisplay.h"
#include "platform/linux/input/X11Input.h"

#include "ProjectIdentity.h"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <string>

namespace platform::linux_desktop
{
namespace
{
std::string upper(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

// The setting, else <PREFIX>_<KEY>, else the fallback (keys the app's settings table does not carry yet).
std::string setting(const Settings& s, const std::string& key, const std::string& fallback)
{
    if (auto it = s.find(key); it != s.end() && ! it->second.empty())
        return it->second;
    return host::envOr(std::string(identity::envPrefix) + "_" + upper(key), fallback);
}

class LinuxPlatform final : public Platform
{
public:
    LinuxPlatform() { window_->title = identity::displayName; }

    std::string_view id() const noexcept override { return "linux"; }

    bool detect() const override
    {
        for (const char* path : {"/dev/subucom_spi0.1", "/proc/asound/ak4458audio"})
            if (::access(path, F_OK) == 0)
                return false; // the reference hardware: imx6
        const char* display = std::getenv("DISPLAY");
        return display != nullptr && *display != 0;
    }

    std::string describe() const override
    {
        const char* display = std::getenv("DISPLAY");
        return "linux desktop (DISPLAY=" + std::string(display != nullptr ? display : "") + "), audio "
             + (filePath_.empty() ? "alsa " + alsaDevice_
                                        + (channels_ != 2 || ! mapNote_.empty()
                                               ? " (" + std::to_string(channels_) + " ch, " + mapNote_ + ", else MAIN = ch 0/1 of 2)"
                                               : " (MAIN = ch 0/1 of 2)")
                                  : "file " + filePath_ + (fileOptions_.allPorts ? " (every port, 12 ch)" : "")
                                        + (fileOptions_.inputPath.empty() ? "" : ", input " + fileOptions_.inputPath))
             + " then offline, X11 window " + std::to_string(800 * window_->scale) + "x"
             + std::to_string(480 * window_->scale) + " (scale " + std::to_string(window_->scale)
             + "), keyboard + mouse touch, lamps " + (lampLog_ ? "logged" : "off");
    }

    Paths paths() const override { return host::xdgPaths(); }

    void configure(const Settings& settings) override
    {
        const std::string device = setting(settings, "audio_device", "default");
        filePath_.clear();
        alsaDevice_ = "default";
        if (device.rfind("file:", 0) == 0)
            filePath_ = device.substr(5);
        else if (device != "hw:ak4458audio,0") // the app's built-in default is the device codec: `default` here
            alsaDevice_ = device;
        fileOptions_.allPorts = setting(settings, "audio_file_ports", "main") == "all";
        const std::string input = setting(settings, "audio_input", "");
        fileOptions_.inputPath = input.rfind("file:", 0) == 0 ? input.substr(5) : std::string();
        window_->scale = std::clamp(std::atoi(setting(settings, "scale", "1").c_str()), 1, 4);
        channels_ = static_cast<unsigned>(std::clamp(std::atoi(setting(settings, "audio_channels", "2").c_str()), 2, 32));
        map_ = common::mainOnly(0, 1);
        mapNote_.clear();
        if (const std::string map = setting(settings, "audio_map", ""); ! map.empty())
            mapNote_ = common::parsePortMap(map, map_) ? "audio_map " + map : "audio_map " + map + " IGNORED (bad syntax)";
        lampLog_ = setting(settings, "lamp_log", "0") == "1";
    }

    std::vector<std::unique_ptr<AudioDevice>> audioDevices() override
    {
        std::vector<std::unique_ptr<AudioDevice>> devices;
        if (! filePath_.empty())
            devices.push_back(std::make_unique<FileAudioDevice>(filePath_, 1.0, fileOptions_));
        else
        {
            if (channels_ != 2 || ! mapNote_.empty()) // OUTPUT CHANNEL MAP (MIX-032), then stereo if it fails
                devices.push_back(std::make_unique<common::AlsaOutput>(
                    common::AlsaOutputConfig {alsaDevice_, channels_, 0, 0, map_}));
            devices.push_back(std::make_unique<common::AlsaOutput>(
                common::AlsaOutputConfig {alsaDevice_, 2, 0, 0, common::mainOnly(0, 1)}));
        }
        devices.push_back(std::make_unique<host::OfflineAudioDevice>()); // nothing else started: keep the app running
        return devices;
    }

    std::vector<std::unique_ptr<io::ControlSource>> controlSources(const ControlEnv& env) override
    {
        std::vector<std::unique_ptr<io::ControlSource>> sources;
        sources.push_back(std::make_unique<X11InputSource>(env.triggers, window_));
        return sources;
    }

    std::unique_ptr<io::LampSink> lamps() override
    {
        return lampLog_ ? std::make_unique<common::LogLamps>() : nullptr;
    }

    std::unique_ptr<DisplayDevice> display(std::string&) override { return makeLinuxDisplay(window_); }

private:
    std::shared_ptr<LinuxWindow> window_ = std::make_shared<LinuxWindow>();
    std::string filePath_, alsaDevice_ = "default";
    unsigned channels_ = 2;
    common::AlsaPortMap map_ = common::mainOnly(0, 1);
    std::string mapNote_;
    FileAudioOptions fileOptions_ {};
    bool lampLog_ = false;
};

std::unique_ptr<Platform> makePlatform() { return std::make_unique<LinuxPlatform>(); }
} // namespace
} // namespace platform::linux_desktop

APP_REGISTER_PLATFORM("linux", &platform::linux_desktop::makePlatform)
