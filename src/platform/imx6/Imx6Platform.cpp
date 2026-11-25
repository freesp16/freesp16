// The reference hardware: NXP i.MX6Q (Cortex-A9 ×4, NEON), AK4458 codec over ALSA, the subucom SPI panel,
// the CDC touch strip, the Vivante GC2000 via gldispatch, an X server without a window manager. Named after the SoC,
// never after the product. Runs on the hardware and in the sp16-emulator (whose hardware shim serves the
// device nodes and the codec PCM to this process).
//
// The same board is the TORAIZ SP-16 and the DJS-1000, found at start (detectUnit) and told apart by:
//   SP-16      AK4458 codec, 16 channels: MAIN, OUT 3-4, 5-6, 7-8; PHONES on hw:ak4452audio,0; the analog filter (its
//              send/return on hw:ak4452audio,1, its select line and HP CUTOFF DAC: AnalogBoard)
//   DJS-1000   MAIN (master) on hw:ak4452audio,1 and PHONES on hw:ak4452audio,0, both stereo S24_LE; no analog filter.
//              Its panel holds the outputs muted until the app's audio runs (LampBus::setOutputsLive), and PHONES
//              follows the PHONES LEVEL knob, which is not analog on this unit.
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
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
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

using panel::Unit;

std::optional<Unit> unitFromKey(std::string_view key) noexcept
{
    if (key == "sp16")
        return Unit::Sp16;
    if (key == "djs1000")
        return Unit::Djs1000;
    return std::nullopt;
}

// The unit: BOOTMENU_UNIT (set by the boot menu), else the official application that is installed, else the SP-16's
// codec, else the SP-16. The setting unit= overrides it (configure()).
Unit detectUnit(std::string& why)
{
    if (const char* env = std::getenv("BOOTMENU_UNIT"); env != nullptr)
        if (const auto u = unitFromKey(env))
        {
            why = std::string("BOOTMENU_UNIT=") + env;
            return *u;
        }
    if (::access("/root/pdj/EP113_Main_R", F_OK) == 0)
        return why = "EP113_Main_R installed", Unit::Djs1000;
    if (::access("/root/pdj/EP085_Main_R", F_OK) == 0)
        return why = "EP085_Main_R installed", Unit::Sp16;
    if (::access("/proc/asound/ak4458audio", F_OK) == 0)
        return why = "AK4458 codec", Unit::Sp16;
    return why = "nothing to tell, assumed", Unit::Sp16;
}

// DJS-1000: its panel keeps the outputs muted while the frames ask for it (LampBus), so the request is released once
// the audio runs and made again before it stops.
class ReleasesMute final : public AudioDevice
{
public:
    ReleasesMute(std::unique_ptr<AudioDevice> inner, std::shared_ptr<panel::LampBus> bus)
        : inner_(std::move(inner)), bus_(std::move(bus))
    {
    }
    ~ReleasesMute() override { stop(); }
    std::string_view id() const noexcept override { return inner_->id(); }
    std::span<const PortInfo> ports() const noexcept override { return inner_->ports(); }
    bool start(const AudioConfig& c, engine::AudioProcessor& p, std::string& info) override
    {
        const bool ok = inner_->start(c, p, info);
        if (ok)
        {
            bus_->setOutputsLive(true);
            info += ", panel mute released";
        }
        return ok;
    }
    void stop() override
    {
        bus_->setOutputsLive(false);
        inner_->stop();
    }
    AudioStats stats() const noexcept override { return inner_->stats(); }

private:
    std::unique_ptr<AudioDevice> inner_;
    std::shared_ptr<panel::LampBus> bus_;
};

class Imx6Platform final : public Platform
{
public:
    Imx6Platform() { setUnit(detectUnit(unitWhy_)); }

    std::string_view id() const noexcept override { return "imx6"; }

    // `/dev/subucom_spi0.1` or the codec exists. In the emulator the device nodes are served by the
    // hardware shim (an LD_PRELOAD library) and do not exist on disk, so the board's backlight node, which the shim
    // serves without side effects, is probed by opening it.
    bool detect() const override { return !detectReason().empty(); }

    std::string describe() const override
    {
        const auto why = detectReason();
        char gains[64];
        std::snprintf(gains, sizeof gains, "main gain %.2f, phones gain %.2f, filter send gain %.2f",
                      static_cast<double>(mainGain_), static_cast<double>(phonesGain_), static_cast<double>(loopGain_));
        const std::string unit = std::string(panel::unitModel(unit_)) + " (" + unitWhy_ + "), ";
        const std::string board = "i.MX6 board (" + (why.empty() ? std::string("not detected") : why) + "), audio ";
        if (unit_ == Unit::Djs1000)
            return unit + board + audioDevice_ + " S24_LE (MAIN = ch 0/1" + (mapNote_.empty() ? "" : "; " + mapNote_)
                 + "), " + gains + ", phones " + (phonesDevice_.empty() ? std::string("off") : phonesDevice_)
                 + (phonesKnob_ ? " at the PHONES LEVEL knob" : "") + ", no analog filter, panel " + panelDevice
                 + " (+ lamps" + (lampLog_ ? ", logged" : "") + ", LCD touch), input trace "
                 + (inputTrace_ ? "on" : "off") + ", strip /dev/cdc_spi2.0, display X11 + Vivante GC2000 (gldispatch)";
        return unit + board + audioDevice_
             + " (MAIN = ch 12/14 of 16; OUT3-4 8/10, OUT5-6 4/6, OUT7-8 0/2" + (mapNote_.empty() ? "" : "; " + mapNote_)
             + "), " + gains + ", phones " + (phonesDevice_.empty() ? std::string("off") : phonesDevice_) + ", filter loop "
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
        // unit=sp16|djs1000: which panel and codec, when the detection is wrong (before anything starts)
        if (auto it = settings.find("unit"); it != settings.end())
            if (const auto u = unitFromKey(it->second); u && *u != unit_)
            {
                unitWhy_ = "setting unit=" + it->second;
                setUnit(*u);
            }
        if (auto it = settings.find("audio_device"); it != settings.end() && ! it->second.empty())
            audioDevice_ = it->second;
        if (auto it = settings.find("input_trace"); it != settings.end()) // INPUT log lines, default on
            inputTrace_ = it->second != "0";
        // The POWER key's bit: low = 0 when pressed (the hardware, default), high = 1 when pressed (the emulator).
        if (auto it = settings.find("power_key"); it != settings.end())
            powerActiveLow_ = it->second != "high";
        // phones_device=<PCM> (default hw:ak4452audio,0; "none" = no headphone stream) and
        // audio_map=<port>:<L>/<R>,… over the codec map (MIX-032).
        if (auto it = settings.find("phones_device"); it != settings.end())
            phonesDevice_ = it->second == "none" ? std::string() : it->second;
        phonesS16_ = false; // phones_format=s16: the 16-bit stream of the first builds (default S24_LE, the reference's)
        if (auto it = settings.find("phones_format"); it != settings.end())
            phonesS16_ = it->second == "s16" || it->second == "S16_LE";
        if (auto it = settings.find("main_gain"); it != settings.end()) // linear, default 0.5 (−6 dB, F111)
            mainGain_ = std::clamp(static_cast<float>(std::strtod(it->second.c_str(), nullptr)), 0.0f, 1.0f);
        if (auto it = settings.find("phones_gain"); it != settings.end()) // linear, default 0.5 (−6 dB, F111)
            phonesGain_ = std::clamp(static_cast<float>(std::strtod(it->second.c_str(), nullptr)), 0.0f, 1.0f);
        // DJS-1000: phones_knob=0 leaves PHONES at phones_gain whatever the PHONES LEVEL knob says (default: the knob)
        if (auto it = settings.find("phones_knob"); it != settings.end())
            phonesKnob_ = it->second != "0";
        // The unit's analog filter is an insert loop on the second AK4452 ("Send & Return", hw:ak4452audio,1), for
        // the MASTER route as for a track: its LP CUTOFF and RESONANCE pots are analog only (no microcontroller reads
        // them), so they act only on audio sent through that loop. ON by default (filter_loop=0 or none = the digital
        // model everywhere: HP CUTOFF on the model, LP CUTOFF dead; another PCM name = that device). The loop PCM takes
        // its clocks from hw:ak4452audio,0 (the PHONES PCM, opened first and started with the main stream); it is not
        // linked to the main PCM (an overrun of a linked capture stopped the whole group on the first unit run), and a
        // return that stays silent hands the slot back to the model (AlsaOutput::readLoop).
        // The board's controls (panel::AnalogBoard: the select line, the limiter line, the HP CUTOFF DAC) follow the
        // loop unless filter_board=0/1 says otherwise; filter_limit=0 leaves the limiter line as the kernel set it.
        // The DJS-1000 has no analog filter, and its hw:ak4452audio,1 is the master: no loop and no board there.
        loopDevice_ = unit_ == Unit::Sp16 ? "hw:ak4452audio,1" : "";
        if (auto it = settings.find("filter_loop"); it != settings.end() && unit_ == Unit::Sp16)
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
        if (auto it = settings.find("filter_board"); it != settings.end() && unit_ == Unit::Sp16)
            board = it->second != "0";
        board_->setEnabled(board);
        board_->setLoop(! loopDevice_.empty());
        if (auto it = settings.find("filter_limit"); it != settings.end())
            board_->setLimit(it->second != "0");
        if (auto it = settings.find("filter_send_gain"); it != settings.end()) // linear, default 0.5 (−6 dB, F111)
            loopGain_ = std::clamp(static_cast<float>(std::strtod(it->second.c_str(), nullptr)), 0.0f, 1.0f);
        map_ = unit_ == Unit::Djs1000 ? common::mainOnly(0, 1) : kCodecMap;
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
        if (unit_ == Unit::Djs1000)
        {
            // MAIN on the master PCM, PHONES on the second one, at the PHONES LEVEL knob
            common::AlsaOutputConfig c {audioDevice_, 2,          period_, periods_, map_, phonesDevice_, 2,
                                        AudioPort::Phones, phonesGain_, phonesS16_, {}, 1.0f, mainGain_};
            c.mainS24 = true;
            if (phonesKnob_)
                c.auxLevel = phonesLevel_;
            devices.push_back(std::make_unique<ReleasesMute>(std::make_unique<common::AlsaOutput>(c), lampBus_));
            return devices;
        }
        // Direct ALSA first; the codec's 16 channels (MAIN = 12/14, the assignable pairs) + PHONES
        devices.push_back(std::make_unique<common::AlsaOutput>(
            common::AlsaOutputConfig {audioDevice_, 16, period_, periods_, map_, phonesDevice_, 2, AudioPort::Phones,
                                      phonesGain_, phonesS16_, loopDevice_, loopGain_, mainGain_}));
        devices.push_back(std::make_unique<AudioOut>());                                    // JUCE ALSA fallback
        return devices;
    }

    std::vector<std::unique_ptr<io::ControlSource>> controlSources(const ControlEnv& env) override
    {
        std::vector<std::unique_ptr<io::ControlSource>> sources;
        // + LCD touch
        auto panel = std::make_unique<PanelLink>(unit_, env.triggers, lampBus_, inputTrace_, powerActiveLow_);
        if (unit_ == Unit::Djs1000)
            panel->setPhonesLevel(phonesLevel_);
        sources.push_back(std::move(panel));
        sources.push_back(std::make_unique<StripSource>(lampBus_, inputTrace_, env.triggers)); // /dev/cdc_spi2.0
        return sources;
    }

    // The panel's TX lamp frames; the panel link sends them (on change + 4 Hz heartbeat, local echo until a flush).
    std::unique_ptr<io::LampSink> lamps() override
    {
        auto panel = std::make_unique<panel::PanelLamps>(lampBus_, unit_ == Unit::Sp16 ? board_ : nullptr);
        if (! lampLog_)
            return panel;
        return std::make_unique<common::TeeLamps>(std::move(panel), std::make_unique<common::LogLamps>());
    }

    std::unique_ptr<DisplayDevice> display(std::string& error) override { return makeDisplay(error); }

    // The four Cortex-A9 cores, split so the display never takes time from audio and MIDI. Before the app starts the
    // system sends every interrupt to core 0 and the audio DMA's to core 1 (/proc/irq/*/smp_affinity, the app's
    // `DIAG irq` lines), and pins its running processes to core 0 — so without this map every thread of the app,
    // started from one of them, would share core 0 with the interrupts (the `DIAG thread` lines show the result):
    //   core 0  the UI / message thread, X, the GL driver, every other interrupt, the workers (with core 3, nice 10)
    //   core 1  the audio thread alone, beside the audio DMA's interrupt
    //   core 2  the small, urgent threads: MIDI clock, MIDI input, the panel link (pads), MIDI out, the touch strip
    //   core 3  the recorder and the workers' second core (room for an engine helper later)
    // The code is locked in RAM (the program runs from a USB stick), the governor held at performance (the engine's
    // budgets assume the full clock), and the real-time limit back at the kernel's default: the system turns it off
    // (−1) before the app starts, so a real-time thread that never sleeps would freeze its core's other threads.
    Tuning tuning() const override
    {
        return {"audio:1,clock:2,midi_in:2,panel:2,midi_out:2,strip:2,recorder:3,ui:0,worker:0+3", true, "performance",
                "950000"};
    }

    void muteForStandby() override { lampBus_->hardMute(); } // SP-16 TX 0x66 bit 0, DJS-1000 0x6c bit 0

    // The stock app's standby: the panel frame's standby flag (TX 0x67 bit 0) with HardMute, every lamp dark; M1 cuts
    // the power.
    bool enterStandby(std::string& info) override
    {
        if (! lampBus_->enterStandby())
        {
            info = "panel link not running: no standby flag";
            return false;
        }
        info = unit_ == Unit::Djs1000 ? "panel standby flag set (TX 0x6c bit 1, mute bit 0), lamps dark"
                                      : "panel standby flag set (TX 0x67 bit 0, HardMute 0x66), lamps dark";
        return true;
    }

private:
    void setUnit(Unit u)
    {
        unit_ = u;
        lampBus_ = std::make_shared<panel::LampBus>(u);
        audioDevice_ = u == Unit::Djs1000 ? "hw:ak4452audio,1" : "hw:ak4458audio,0";
        map_ = u == Unit::Djs1000 ? common::mainOnly(0, 1) : kCodecMap;
    }

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
    float mainGain_ = 0.5f;                         // F111's fixed −6 dB on the main and assignable DAC pairs
    float phonesGain_ = 0.5f;                       // F111's fixed −6 dB on the headphone DAC
    bool phonesS16_ = false;
    std::string loopDevice_;                        // the analog filter's send/return (default hw:ak4452audio,1)
    std::string loopNote_;
    float loopGain_ = 0.5f;                         // F111's fixed −6 dB on the filter send DAC
    std::shared_ptr<panel::AnalogBoard> board_ = std::make_shared<panel::AnalogBoard>();
    common::AlsaPortMap map_ = kCodecMap;
    std::string mapNote_;
    bool inputTrace_ = true;
    bool powerActiveLow_ = true; // power_key=low|high
    bool lampLog_ = false;
    unsigned period_ = 0, periods_ = 0; // 0 = AudioConfig's (256 × 3)
    Unit unit_ = Unit::Sp16;
    std::string unitWhy_;
    bool phonesKnob_ = true; // DJS-1000: PHONES follows its PHONES LEVEL knob
    // DJS-1000: the PHONES LEVEL knob as a gain, written by the panel's reader thread, read by the audio thread. A
    // quarter until the panel's first frame brings the knob's position.
    std::shared_ptr<std::atomic<float>> phonesLevel_ = std::make_shared<std::atomic<float>>(0.25f);
    std::shared_ptr<panel::LampBus> lampBus_; // panel link + lamps + strip echo (setUnit)
};

std::unique_ptr<Platform> makePlatform() { return std::make_unique<Imx6Platform>(); }
} // namespace
} // namespace platform::imx6

APP_REGISTER_PLATFORM("imx6", &platform::imx6::makePlatform)
