// The reference hardware: NXP i.MX6Q (Cortex-A9 ×4, NEON), AK4458 codec over ALSA, the subucom SPI panel,
// the CDC touch strip, the Vivante GC2000 via gldispatch, an X server without a window manager. Named after the SoC,
// never after the product. Runs on the hardware and in the sp16-emulator (whose hardware shim serves the
// device nodes and the codec PCM to this process).
#include "platform/Platform.h"

#include "platform/common/LogLamps.h"
#include "platform/common/audio/AlsaOutput.h"
#include "platform/imx6/audio/AudioOut.h"
#include "platform/imx6/display/Imx6Display.h"
#include "platform/imx6/panel/AnalogBoard.h"
#include "platform/imx6/panel/PanelLamps.h"
#include "platform/imx6/panel/PanelLink.h"
#include "platform/imx6/strip/StripSource.h"

#include "ProjectIdentity.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>

namespace platform::imx6
{
namespace
{
constexpr const char* panelDevice = "/dev/subucom_spi0.1";

// The codec's logical outputs (MIX-032; sp16-emulator docs/audio-jack-map.md): 16 S16 channels
// in 8 two-channel DAC slots where only the even channel carries audio, MAIN = 12/14 (grade O, observed), OUT3-4 =
// 8/10 and OUT5-6 = 4/6 (grade I, inferred), OUT7-8 = 0/2; the odd channels are padding written as 0. PHONES is the
// separate PCM hw:ak4452audio,0 channels 0/1 (AlsaOutputConfig::auxDevice); the AFS send channel is unknown.
// The map is data: the setting audio_map (e.g. "out34:8/10,out56:4/6") corrects it without a rebuild.
constexpr common::AlsaPortMap kCodecMap {{{AudioPort::Main, 12, 14},
                                         {AudioPort::Out34, 8, 10},
                                         {AudioPort::Out56, 4, 6},
                                         {AudioPort::Out78, 0, 2},
                                         {AudioPort::Phones, -1, -1},
                                         {AudioPort::FilterSend, -1, -1}}};

class Imx6Platform final : public Platform
{
public:
    std::string_view id() const noexcept override { return "imx6"; }

    // `/dev/subucom_spi0.1` or the codec exists. In the emulator the device nodes are served by the
    // hardware shim (an LD_PRELOAD library) and do not exist on disk, so the board's backlight node, which the shim
    // serves without side effects, is probed by opening it.
    bool detect() const override { return !detectReason().empty(); }

    std::string describe() const override
    {
        const auto why = detectReason();
        return "i.MX6 board (" + (why.empty() ? std::string("not detected") : why) + "), audio " + audioDevice_
             + " (MAIN = ch 12/14 of 16; OUT3-4 8/10, OUT5-6 4/6, OUT7-8 0/2" + (mapNote_.empty() ? "" : "; " + mapNote_)
             + "), phones " + (phonesDevice_.empty() ? std::string("off") : phonesDevice_) + ", filter loop "
             + (loopDevice_.empty() ? std::string("off (digital model)") : loopDevice_ + loopNote_) + ", filter board "
             + (board_->enabled() ? "on" : "off") + ", panel " + panelDevice
             + " (+ lamps" + (lampLog_ ? ", logged" : "") + ", LCD touch), input trace " + (inputTrace_ ? "on" : "off")
             + ", strip /dev/cdc_spi2.0"
             + ", display X11 + Vivante GC2000 (gldispatch)";
    }

    Paths paths() const override
    {
        // Settings: key=value lines in the dev-overlay share (the skeleton's rule, read-only inside the app's root);
        // Without that overlay (a real unit), <data dir>/<name>.conf — on the USB-stick launcher's
        // session the stick's <name>-data/<name>/<name>.conf, which the app re-reads on every start (the launcher's
        // own stick-root settings file reaches the app as <PREFIX>_<KEY> variables, read once per stick insertion).
        Paths p;
        p.dataDir = std::string("/root/data/") + identity::name; // data volume (our choice)
        p.cacheDir = p.dataDir + "/cache";
        const std::string overlay = std::string("/opt/sp16/dev-share/") + identity::name + ".conf";
        p.settingsFile = ::access(overlay.c_str(), F_OK) == 0 ? overlay : p.dataDir + "/" + identity::name + ".conf";
        return p;
    }

    void configure(const Settings& settings) override
    {
        if (auto it = settings.find("audio_device"); it != settings.end() && ! it->second.empty())
            audioDevice_ = it->second;
        if (auto it = settings.find("input_trace"); it != settings.end()) // INPUT log lines, default on
            inputTrace_ = it->second != "0";
        // phones_device=<PCM> (default hw:ak4452audio,0; "none" = no headphone stream) and
        // audio_map=<port>:<L>/<R>,… over the codec map (MIX-032).
        if (auto it = settings.find("phones_device"); it != settings.end())
            phonesDevice_ = it->second == "none" ? std::string() : it->second;
        phonesS16_ = false; // phones_format=s16: the 16-bit stream of the first builds (default S24_LE, the reference's)
        if (auto it = settings.find("phones_format"); it != settings.end())
            phonesS16_ = it->second == "s16" || it->second == "S16_LE";
        if (auto it = settings.find("phones_gain"); it != settings.end()) // linear, default 0.5 (−6 dB, F111)
            phonesGain_ = std::clamp(static_cast<float>(std::strtod(it->second.c_str(), nullptr)), 0.0f, 1.0f);
        // The unit's analog filter is an insert loop on the second AK4452 ("Send & Return", hw:ak4452audio,1), for
        // the MASTER route as for a track: its LP CUTOFF and RESONANCE pots are analog only (no microcontroller reads
        // them), so they act only on audio sent through that loop. ON by default (filter_loop=0 or none = the digital
        // model everywhere: HP CUTOFF on the model, LP CUTOFF dead; another PCM name = that device). The loop PCM takes
        // its clocks from hw:ak4452audio,0 (the PHONES PCM, opened first and started with the main stream); it is not
        // linked to the main PCM (an overrun of a linked capture stopped the whole group on the first unit run), and a
        // return that stays silent hands the slot back to the model (AlsaOutput::readLoop).
        // The board's controls (panel::AnalogBoard: the select line, the limiter line, the HP CUTOFF DAC) follow the
        // loop unless filter_board=0/1 says otherwise; filter_limit=0 leaves the limiter line as the kernel set it.
        loopDevice_ = "hw:ak4452audio,1";
        if (auto it = settings.find("filter_loop"); it != settings.end())
        {
            const std::string& v = it->second;
            if (v == "0" || v == "none" || v.empty())
                loopDevice_.clear();
            else if (v != "1")
                loopDevice_ = v;
        }
        if (phonesDevice_.empty() && loopDevice_ == "hw:ak4452audio,1")
            loopNote_ = " (no PHONES PCM: the loop may have no clock)";
        else
            loopNote_.clear();
        bool board = ! loopDevice_.empty();
        if (auto it = settings.find("filter_board"); it != settings.end())
            board = it->second != "0";
        board_->setEnabled(board);
        board_->setLoop(! loopDevice_.empty());
        if (auto it = settings.find("filter_limit"); it != settings.end())
            board_->setLimit(it->second != "0");
        if (auto it = settings.find("filter_send_gain"); it != settings.end()) // linear, default 0.5 (−6 dB, F111)
            loopGain_ = std::clamp(static_cast<float>(std::strtod(it->second.c_str(), nullptr)), 0.0f, 1.0f);
        map_ = kCodecMap;
        mapNote_.clear();
        if (auto it = settings.find("audio_map"); it != settings.end() && ! it->second.empty())
            mapNote_ = common::parsePortMap(it->second, map_) ? "audio_map " + it->second
                                                              : "audio_map " + it->second + " IGNORED (bad syntax)";
        // audio_period / audio_periods from the settings file too (<PREFIX>_AUDIO_PERIOD still wins in
        // AlsaOutput), and lamp_log=1 logs the lamp frames beside the panel's lamps.
        auto number = [&](const char* key) {
            auto it = settings.find(key);
            return it == settings.end() ? 0u : static_cast<unsigned>(std::strtoul(it->second.c_str(), nullptr, 10));
        };
        period_ = number("audio_period");
        periods_ = number("audio_periods");
        if (auto it = settings.find("lamp_log"); it != settings.end())
            lampLog_ = it->second == "1";
    }

    std::vector<std::unique_ptr<AudioDevice>> audioDevices() override
    {
        std::vector<std::unique_ptr<AudioDevice>> devices;
        // Direct ALSA first; the codec's 16 channels (MAIN = 12/14, the assignable pairs) + PHONES
        devices.push_back(std::make_unique<common::AlsaOutput>(
            common::AlsaOutputConfig {audioDevice_, 16, period_, periods_, map_, phonesDevice_, 2, AudioPort::Phones,
                                      phonesGain_, phonesS16_, loopDevice_, loopGain_}));
        devices.push_back(std::make_unique<AudioOut>());                                    // JUCE ALSA fallback
        return devices;
    }

    std::vector<std::unique_ptr<io::ControlSource>> controlSources(const ControlEnv& env) override
    {
        std::vector<std::unique_ptr<io::ControlSource>> sources;
        sources.push_back(std::make_unique<PanelLink>(env.triggers, lampBus_, inputTrace_)); // + LCD touch
        sources.push_back(std::make_unique<StripSource>(lampBus_, inputTrace_, env.triggers)); // /dev/cdc_spi2.0
        return sources;
    }

    // The panel's TX lamp frames; the panel link sends them (on change + 4 Hz heartbeat, local echo until a flush).
    std::unique_ptr<io::LampSink> lamps() override
    {
        auto panel = std::make_unique<panel::PanelLamps>(lampBus_, board_);
        if (! lampLog_)
            return panel;
        return std::make_unique<common::TeeLamps>(std::move(panel), std::make_unique<common::LogLamps>());
    }

    std::unique_ptr<DisplayDevice> display(std::string& error) override { return makeDisplay(error); }

private:
    static std::string detectReason()
    {
        for (const char* path : {panelDevice, "/proc/asound/ak4458audio", "/dev/cdc_spi2.0"})
            if (::access(path, F_OK) == 0)
                return std::string(path) + " present";
        const int fd = ::open("/dev/backlight", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0)
        {
            ::close(fd);
            return "/dev/backlight opens";
        }
        return {};
    }

    std::string audioDevice_ = "hw:ak4458audio,0";
    std::string phonesDevice_ = "hw:ak4452audio,0";
    float phonesGain_ = 0.5f;                       // F111's fixed −6 dB on the headphone DAC
    bool phonesS16_ = false;
    std::string loopDevice_;                        // the analog filter's send/return (default hw:ak4452audio,1)
    std::string loopNote_;
    float loopGain_ = 0.5f;                         // F111's fixed −6 dB on the filter send DAC
    std::shared_ptr<panel::AnalogBoard> board_ = std::make_shared<panel::AnalogBoard>();
    common::AlsaPortMap map_ = kCodecMap;
    std::string mapNote_;
    bool inputTrace_ = true;
    bool lampLog_ = false;
    unsigned period_ = 0, periods_ = 0; // 0 = AudioConfig's (256 × 3)
    std::shared_ptr<panel::LampBus> lampBus_ = std::make_shared<panel::LampBus>(); // panel link + lamps + strip echo
};

std::unique_ptr<Platform> makePlatform() { return std::make_unique<Imx6Platform>(); }
} // namespace
} // namespace platform::imx6

APP_REGISTER_PLATFORM("imx6", &platform::imx6::makePlatform)
