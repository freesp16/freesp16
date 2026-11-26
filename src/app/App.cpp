// The app (composition root). Behaviour and log lines are the walking skeleton's:
//
//  1. Select the platform (platform::select: "auto" → imx6 on the board and in the emulator, host elsewhere), read the
//     settings from the platform's settings file + environment, open the display.
//  2. Build the glyph atlas for the screen size (one-off JUCE font work; no threads, no X connection yet).
//  3. GL backends (registered with needsChildProbe), each in a forked child: bind the driver flavour, create the
//     backend, draw one HOME frame (compare=1: `compare_frames` frames and frame-time statistics). A crash, hang or
//     missing GPU only rules that backend out, and the parent never loads a driver flavour it does not run with.
//  4. CPU backends (soft = custom rasteriser, juce = JUCE software Graphics) are measured in the parent (compare=1 only).
//  5. Run with the first working backend of `render`: engine::createEngine() on the first platform audio device that
//     starts; the platform's control sources (pad hits go straight to the engine, UI-118) → ui::createRoot() (HOME) →
//     DrawList → renderer, once per frame.
//  6. The document (app/Document.h): model + CommandBus (edits → engine through the bridge hook), sample library and
//     pool, project session (resume, autosave), pad controller; per frame before the root's frame.
//
// Settings (environment <PREFIX>_<KEY>, or key=value lines in the platform's settings file — on imx6
// /opt/sp16/dev-share/<name>.conf of a dev overlay):
//   platform=auto|imx6|host          (default auto; the environment variable wins over the file)
//   render=auto|gl-fb|gl-x11|juce-gl|soft|juce[,...]  order to try (auto = the display's order; imx6: gl-fb,gl-x11,juce-gl,soft)
//   compare=0|1       benchmark every backend first (default 0: GL backends are still probed in a child, one frame,
//                     but nothing is measured; the emulator smoke passes compare=1)      compare_frames=N   (default 300)
//   audio=0|1         (default 1)       audio_device=hw:ak4458audio,0 (imx6: direct ALSA; falls back to JUCE ALSA)
//   run_frames=N      stop after N frames (default 0 = until SIGTERM/SIGINT; the host smoke test uses it)
//   dev_kit=<folder>  development: load the folder's first 16 audio files onto tracks 1–16 at start-up (relative =
//                     under the data directory; the emulator smoke's generated kit), "KIT ready" once installed
//   dev_kit_scenes=N  … onto scenes 1–N (default 1; the scene/pattern check uses 4)
//   input_script=FILE  screenshot=FILE.png  screenshot_frame=N   diagnostics (app/Screenshot.h)
//   midi=0|1  midi_providers=a,b  midi_virtual=FILE  midi_virtual_out=FILE  midi_spin_us=N   MIDI (io/midi/MidiSource.cpp)
//   diag_interval=S   seconds between the FRAME / LOAD / AUDIO lines (default 10; 0 = only at STOP)
//   internal_disk=DIR the reference hardware's internal disk mount (default /mnt/emmc; a read-only library
//                     location, `cmd import-sp16 internal:…`)
// Every <PREFIX>_<KEY> variable and every settings-file line reaches the settings (app/SettingsFile.h:
// keys lower case, "__" = "."), not only the keys above; one `SETTINGS …` line after START lists what differs from
// these defaults. Then one block of `DIAG …` lines (app/Diagnostics.h: platform and its detection, kernel, CPUs, C
// library, memory, data dir + free space, FAT mounts and the internal disk, sound cards / PCMs / MIDI ports, the audio
// device's format and port map, every input source, the display backend and GL strings, the settings file), ending
// in `DIAG END`; `DIAG STOP …` sums the run up at exit. stdout is line-buffered and a fatal signal writes `CRASH …`
// lines with a backtrace before the process dies (the device log on the USB stick stays complete).
// Diagnostic commands (scripted input `cmd …`, handled here): `mark <label>` logs "MARK <label> t=<s>" (the phases of
// the hardware-test scripts, tools/hw-test-bundle.py); `diag` prints the periodic lines now; `diag-crash segv|abort`
// crashes on purpose (to check the CRASH report on a unit).
//   controllers=<port glob>[:<profile>],…  ports that are MIDI pad controllers (io/controllers/profiles/ControllerHub.h)
//   virtual_panel=hidden|collapsed|expanded   the on-screen panel's start state (io/controllers/VirtualPanel.h)
// Every log line starts with "[sp16-mod] <PREFIX>": the emulator copies those lines into the session's console.txt.
#include "app/App.h"
#include "app/Diagnostics.h"
#include "app/Screenshot.h"
#include "app/SettingsFile.h"
#include "app/SystemTuning.h"
#include "app/UiThrottle.h"

#include "app/Document.h"
#include "core/params/catalogue/fx.h"
#include "ProjectIdentity.h"
#include "core/params/ParamRegistry.h"
#include "core/util/ThreadRole.h"
#include "services/library/SampleLibrary.h"
#include "engine/EngineApi.h"
#include "io/controllers/AppRequests.h"
#include "io/controllers/LampRules.h"
#include "io/controllers/VirtualPanel.h"
#include "io/controllers/profiles/ControllerHub.h"
#include "io/midi/MidiSettings.h"
#include "platform/Platform.h"
#include "services/settings/Preferences.h"
#include "ui/render/GlyphAtlas.h"
#include "ui/render/RendererRegistry.h"
#include "ui/screens/virtualpanel/PanelView.h"
#include "ui/shell/Root.h"

#include <juce_events/juce_events.h>

#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

namespace app
{
namespace
{
std::atomic<bool> quitRequested {false};

std::string residentKb() // VmRSS from /proc/self/status (Linux, the device); "n/a" without /proc (host, emulator chroot)
{
    const long kb = diag::residentKb();
    return kb < 0 ? std::string("n/a") : std::to_string(kb) + " kB";
}

void logLine(const char* fmt, ...)
{
    char buf[1200];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::printf("[sp16-mod] %s %s\n", identity::envPrefix, buf);
    std::fflush(stdout);
}

double nowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

std::string envName(std::string_view key)
{
    std::string env = std::string(identity::envPrefix) + "_" + std::string(key);
    std::transform(env.begin(), env.end(), env.begin(), ::toupper);
    return env;
}

// The defaults; any other key may come from the settings file or the environment as well (app/SettingsFile.h).
const platform::Settings& defaultSettings()
{
    static const platform::Settings s {{"platform", "auto"}, {"render", "auto"}, {"compare", "0"},
                                       {"compare_frames", "300"}, {"audio", "1"}, {"audio_device", "hw:ak4458audio,0"},
                                       {"run_frames", "0"}, {"dev_kit", ""}, {"dev_kit_scenes", "1"},
                                       {"diag_interval", "10"}, {"internal_disk", "/mnt/emmc"},
                                       {"test_tone", "0"}};
    return s;
}

platform::Settings readSettings(const std::string& file, SettingsOrigin* origin = nullptr)
{
    return app::loadSettings(defaultSettings(), file, identity::envPrefix, nullptr, origin);
}

// The preferences the devices need before they open (UTILITY, services/settings/Preferences.h), read
// here from <data dir>/preferences.json without side effects (the document loads the file again for everything else,
// and reports or moves aside a damaged one). Only values the user has set count; the settings file and the
// environment still win (audio_period, scale, audio_map).
//   AUDIO DEVICE › BUFFER SIZE 64 … 512   → AudioConfig::periodFrames (every platform; default 256 = AudioConfig's)
//   UI SCALE 100 … 200 %                  → linux window scale (rounded to a whole factor: 100/125 → 1, ≥ 150 → 2)
//   OUTPUT CHANNEL MAP (MIX-032)          → linux audio_map + audio_channels (pair n = channels 2n−2 / 2n−1, OFF =
//                                           absent). Not on imx6: its codec channels are wired jacks (12/14, 8/10,
//                                           …), not the interface pairs the page lists. AFS RETURN (an input)
//                                           has no device mapping yet.
struct StartPrefs
{
    unsigned periodFrames = 0; // 0 = not set
    int windowScale = 0;       // 0 = not set
    std::string audioMap;      // "" = not set
    unsigned channels = 0;
    std::string note;
};

StartPrefs readStartPrefs(const std::string& dataDir)
{
    namespace st = services::settings;
    StartPrefs out;
    const auto file = juce::File(juce::String::fromUTF8(dataDir.c_str())).getChildFile("preferences.json");
    if (! file.existsAsFile())
    {
        out.note = "no preferences file";
        return out;
    }
    st::Preferences prefs(core::builtinParams());
    if (! prefs.fromJson(file.loadFileAsString()))
    {
        out.note = "preferences unreadable (defaults)";
        return out;
    }
    const auto key = [](std::string_view id)
    {
        const auto* spec = core::builtinParams().find(id);
        return spec != nullptr ? st::keyOf(*spec) : std::string(id);
    };
    const auto value = [&](std::string_view id) { return static_cast<int>(std::lround(prefs.get(key(id)))); };
    static constexpr unsigned kBuffers[] = {64, 128, 256, 512}; // the BUFFER SIZE labels (catalogue/settings.cpp)
    if (prefs.has(key("audio.bufferSize")))
        out.periodFrames = kBuffers[std::clamp(value("audio.bufferSize"), 0, 3)];
    if (prefs.has(key("ui.scale")))
        out.windowScale = std::clamp((value("ui.scale") + 50) / 100, 1, 2);
    static constexpr std::pair<std::string_view, std::string_view> kPorts[] = {
        {"audio.out.main", "main"},     {"audio.out.assign34", "out34"}, {"audio.out.assign56", "out56"},
        {"audio.out.assign78", "out78"}, {"audio.out.phones", "phones"},  {"audio.out.afsSend", "filtersend"}};
    bool mapSet = false;
    for (const auto& [id, port] : kPorts)
        mapSet = mapSet || prefs.has(key(id));
    if (mapSet)
    {
        int pairs = 1;
        for (const auto& [id, port] : kPorts)
        {
            const int pair = std::clamp(value(id), 0, 8); // 0 = OFF, n = channels 2n−2 / 2n−1
            pairs = std::max(pairs, pair);
            out.audioMap += (out.audioMap.empty() ? "" : ",") + std::string(port) + ":"
                          + (pair > 0 ? std::to_string(2 * pair - 2) + "/" + std::to_string(2 * pair - 1) : "-1/-1");
        }
        out.channels = static_cast<unsigned>(2 * pairs);
    }
    return out;
}

std::vector<std::string> split(const std::string& s)
{
    std::vector<std::string> out;
    std::stringstream ss(s);
    for (std::string item; std::getline(ss, item, ',');)
        if (! item.empty())
            out.push_back(item);
    return out;
}

struct Stats
{
    std::vector<double> v;
    double pct(double p) const
    {
        if (v.empty())
            return 0;
        auto s = v;
        const auto k = static_cast<std::size_t>(std::min<double>(static_cast<double>(s.size() - 1), p * static_cast<double>(s.size())));
        std::nth_element(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(k), s.end());
        return s[k];
    }
    double mean() const
    {
        double t = 0;
        for (auto x : v)
            t += x;
        return v.empty() ? 0 : t / static_cast<double>(v.size());
    }
    double max() const { return v.empty() ? 0 : *std::max_element(v.begin(), v.end()); }
};

struct FrameStats
{
    Stats build, raster, present, total;
    void add(double b, const ui::FrameTiming& t)
    {
        build.v.push_back(b);
        raster.v.push_back(t.rasterMs);
        present.v.push_back(t.presentMs);
        total.v.push_back(b + t.rasterMs + t.presentMs);
    }
    void print(const char* label, std::string_view backend) const
    {
        logLine("FRAME %s backend=%.*s frames=%zu build_ms avg=%.2f p95=%.2f | raster_ms avg=%.2f p95=%.2f | present_ms "
                "avg=%.2f p95=%.2f | total_ms avg=%.2f p50=%.2f p95=%.2f max=%.2f",
                label, static_cast<int>(backend.size()), backend.data(), total.v.size(), build.mean(), build.pct(0.95),
                raster.mean(), raster.pct(0.95), present.mean(), present.pct(0.95), total.mean(), total.pct(0.5),
                total.pct(0.95), total.max());
    }
};

ui::TextMeasure measureWith(const ui::GlyphAtlas& atlas)
{
    return [&atlas](ui::TextStyleId s, std::string_view t) { return atlas.measure(s, t) / atlas.scale(); };
}

// The comparison draws HOME with the MASTER meter at −12 dBFS (the test tone's level) and no engine attached.
class MeasureBridge final : public engine::EngineBridge
{
public:
    MeasureBridge() { meters_.masterPeak = {0.2512f, 0.2512f}; }
    bool post(const engine::EngineMessage&) override { return false; }
    void apply(const core::ChangeSet&, const engine::model::Project&) override {}
    const engine::PlayheadFeed& playhead() const override { return playhead_; }
    const engine::MeterFeed& meters() const override { return meters_; }
    void retirePending() override {}
    bool popEvent(engine::EngineEvent&) override { return false; }

private:
    engine::PlayheadFeed playhead_ {};
    engine::MeterFeed meters_ {};
};

// Draws `frames` HOME frames without input (pads flash in turn so the frame is not static); used by the comparison.
bool measureFrames(ui::Renderer& r, const ui::GlyphAtlas& atlas, int frames, FrameStats& stats)
{
    MeasureBridge bridge;
    auto root = ui::createRoot({core::builtinParams(), measureWith(atlas), &bridge, {}});
    ui::DrawList dl;
    dl.reserve(2048, 16384);
    for (int i = 0; i < frames && ! quitRequested; ++i)
    {
        const double t0 = nowMs();
        if (i % 20 == 0)
            root->handle({io::ControlEvent::Kind::Press, io::pad(1 + (i / 20) % 16), 100, io::kSourceVirtual, t0}, t0);
        dl.clear();
        root->frame(dl, t0);
        const double built = nowMs() - t0;
        ui::FrameTiming t;
        if (! r.render(dl, t))
            return false;
        stats.add(built, t);
    }
    return true;
}

// Power off (the panel's POWER/STANDBY key, io::requestStandby): unsaved changes are autosaved, the resume state and
// the preferences written (Document::shutdown(true)) and the audio has stopped. Flush the file systems (the data is on the USB stick), then hand the machine to its power
// controller and wait with the control sources still running: on the imx6 the panel link keeps sending the standby
// flag until the unit's M1 cuts the power, as after the stock app's standby. If the power stays on (no power controller,
// e.g. the host or the emulator, or the flag was ignored), the app quits as usual and the boot menu comes back.
constexpr double kStandbyWaitMs = 20000.0;

void powerOff(platform::Platform& machine)
{
    ::sync();
    std::string info;
    if (! machine.enterStandby(info))
    {
        logLine("STANDBY %.*s has no power controller%s%s: quitting", static_cast<int>(machine.id().size()),
                machine.id().data(), info.empty() ? "" : ": ", info.c_str());
        return;
    }
    logLine("STANDBY %s; waiting for the power to go", info.c_str());
    ::sync(); // the log line too
    const double start = nowMs();
    while (! quitRequested && nowMs() - start < kStandbyWaitMs)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    logLine("STANDBY the power stayed on for %.1f s: quitting", (nowMs() - start) / 1000.0);
}

std::string upper(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), ::toupper);
    return out;
}
} // namespace

int run()
{
    diag::lineBufferStdout(); // the device log (a file on the USB stick) complete after a crash
    {
        char tag[64];
        std::snprintf(tag, sizeof tag, "[sp16-mod] %s", identity::envPrefix);
        diag::installCrashHandler(tag);
    }
    signal(SIGTERM, [](int) { quitRequested = true; });
    signal(SIGINT, [](int) { quitRequested = true; });
    const double runStart = nowMs();

    // 1. Platform, settings, display.
    const char* forced = std::getenv(envName("platform").c_str());
    std::string platformLog;
    auto machine = platform::select(forced != nullptr && *forced != 0 ? forced : "auto", platformLog);
    if (! machine)
    {
        logLine("FATAL %s", platformLog.c_str());
        return 1;
    }
    SettingsOrigin origin;
    auto settings = readSettings(machine->paths().settingsFile, &origin);
    if (forced == nullptr && settings["platform"] != "auto" && settings["platform"] != machine->id())
    {
        std::string again;
        if (auto other = platform::select(settings["platform"], again); other && other->id() == settings["platform"])
        {
            machine = std::move(other);
            platformLog = again + " (settings file)";
            settings = readSettings(machine->paths().settingsFile, &origin);
        }
    }
    const platform::Settings started = settings; // before configureDiagnostics adds its keys: what the SETTINGS line shows
    // File keys reach code that reads its own <PREFIX>_<KEY> variables too (AlsaOutput's period, the MIDI sources).
    app::exportSettingsToEnvironment(started, defaultSettings(), identity::envPrefix);
    // UTILITY preferences the devices need before they open (readStartPrefs); explicit settings win.
    const StartPrefs startPrefs = readStartPrefs(machine->paths().dataDir);
    std::string startPrefsLine;
    {
        const bool onLinux = machine->id() == "linux";
        const auto fromPrefs = [&](const char* key, const std::string& v, bool platformOk)
        {
            if (v.empty())
                return;
            const bool explicitSetting = settings.count(key) != 0 || std::getenv(envName(key).c_str()) != nullptr;
            if (platformOk && ! explicitSetting)
                settings[key] = v;
            startPrefsLine += std::string(" ") + key + "=" + v
                            + (explicitSetting ? " (setting wins)" : platformOk ? "" : " (not on this platform)");
        };
        fromPrefs("scale", startPrefs.windowScale > 0 ? std::to_string(startPrefs.windowScale) : std::string(), onLinux);
        fromPrefs("audio_map", startPrefs.audioMap, onLinux);
        if (onLinux && ! startPrefs.audioMap.empty() && settings["audio_map"] == startPrefs.audioMap)
            settings["audio_channels"] = std::to_string(startPrefs.channels);
        if (startPrefs.periodFrames > 0)
            startPrefsLine += " buffer=" + std::to_string(startPrefs.periodFrames);
        if (! startPrefs.note.empty())
            startPrefsLine += " (" + startPrefs.note + ")";
    }
    configureDiagnostics(settings); // input_script, screenshot (app/Screenshot.h)
    io::midi::configure(settings);  // midi, midi_providers, midi_virtual… (io/midi/MidiSettings.h)
    machine->configure(settings);
    // Thread roles and system tuning (platform::Tuning; core/util/ThreadRole.h), before any thread of ours starts: the
    // platform's map, then thread_map over it ("none" = no map); this thread takes the UI role first, so every thread
    // started from it without a role of its own (JUCE, X, the GL driver) inherits the UI's cores.
    const platform::Tuning tuning = machine->tuning();
    const auto setting = [&](const char* key)
    {
        const auto it = settings.find(key);
        return it == settings.end() ? std::string() : it->second;
    };
    std::string threadNote;
    core::threads::reset();
    if (setting("thread_map") != "none")
    {
        core::threads::configure(tuning.threadMap, threadNote);
        core::threads::configure(setting("thread_map"), threadNote);
    }
    const auto uiRole = core::threads::apply(core::threads::Role::Ui);
    const std::string threadMapLine = "threads map " + core::threads::describe() + " origin="
                                    + (setting("thread_map") == "none" ? std::string("none (thread_map=none)")
                                       : setting("thread_map").empty() ? std::string("platform")
                                                                       : "thread_map " + setting("thread_map"))
                                    + " ui_pinned=" + (uiRole.pinned ? "yes" : "no")
                                    + (threadNote.empty() ? "" : " IGNORED " + threadNote);
    const std::string governorWanted = setting("cpu_governor").empty() ? tuning.governor
                                     : setting("cpu_governor") == "keep" ? std::string()
                                                                         : setting("cpu_governor");
    const auto governor = app::tuning::setGovernor(governorWanted);
    const std::string rtRuntimeWanted = setting("rt_runtime_us").empty() ? tuning.rtRuntime
                                      : setting("rt_runtime_us") == "keep" ? std::string()
                                                                           : setting("rt_runtime_us");
    const auto rtRuntime = app::tuning::setRtRuntime(rtRuntimeWanted);
    struct RestoreTuning // the previous governor and real-time limit come back on every way out of this function
    {
        const app::tuning::Governor& g;
        const app::tuning::RtRuntime& rt;
        ~RestoreTuning()
        {
            app::tuning::restore(rt);
            app::tuning::restore(g);
        }
    } restoreTuning {governor, rtRuntime};
    const bool lockCode = setting("mem_lock").empty() ? tuning.lockCode : setting("mem_lock") == "1";
    logLine("START %s %s (%s) JUCE %s render=%s compare=%s pid=%d", identity::displayName, identity::version,
            identity::id, juce::SystemStats::getJUCEVersion().toRawUTF8(), settings["render"].c_str(),
            settings["compare"].c_str(), static_cast<int>(getpid()));
    logLine("PLATFORM %s | %s", platformLog.c_str(), machine->describe().c_str());
    logLine("SETTINGS %s", describeSettings(started, defaultSettings(), origin).c_str());
    logLine("PREFS start-up%s", startPrefsLine.empty() ? " (defaults)" : startPrefsLine.c_str());
    // The DIAG block (app/Diagnostics.h). The system part now, before anything can fail.
    const auto diagLine = [](const std::string& line) { logLine("DIAG %s", line.c_str()); };
    const platform::Paths paths = machine->paths();
    diagLine("platform " + std::string(machine->id()) + " reason: " + platformLog + " | " + machine->describe());
    diagLine("settings file=" + origin.file + (origin.fileFound ? " (read, " : " (missing, ")
             + std::to_string(origin.fileKeys) + " keys) env_keys=" + std::to_string(origin.envKeys));
    diag::systemLines(paths, diagLine);
    diagLine(threadMapLine);
    diagLine(app::tuning::describe(governor, governorWanted));
    diagLine(app::tuning::describe(rtRuntime, rtRuntimeWanted));
    diag::irqLines(diagLine);
    {
        const juce::File internal(juce::String::fromUTF8(settings["internal_disk"].c_str()));
        if (internal.isDirectory())
            diagLine("storage internal=" + settings["internal_disk"] + " samples="
                     + services::library::InternalDisk::samplesRoot(internal).getFullPathName().toStdString()
                     + " projects=" + std::to_string(services::library::InternalDisk::projects(internal).size())
                     + " free=" + std::to_string(diag::freeBytes(settings["internal_disk"]) / (1024 * 1024))
                     + " MB (read-only library location " + services::library::InternalDisk::kAlias + ")");
        else
            diagLine("storage internal=" + settings["internal_disk"] + " absent");
    }
    const long diagIntervalS = std::max(0L, std::atol(settings["diag_interval"].c_str()));

    std::string error;
    auto display = machine->display(error);
    if (! display)
    {
        logLine("FATAL no display: %s", error.c_str());
        logLine("DIAG STOP reason=fatal uptime_s=%.1f", (nowMs() - runStart) / 1000.0);
        return 1;
    }
    const int screenW = display->width(), screenH = display->height();
    if (! display->sizeKnown())
        logLine("no X display; assuming %dx%d", screenW, screenH);

    // 2. Glyph atlas at the screen's scale.
    ui::GlyphAtlas atlas;
    const double atlasStart = nowMs();
    if (! atlas.loadOrBuild(ui::CanvasTransform::fit(screenW, screenH).scale, machine->paths().cacheDir, error))
    {
        logLine("FATAL %s", error.c_str());
        logLine("DIAG STOP reason=fatal uptime_s=%.1f", (nowMs() - runStart) / 1000.0);
        return 1;
    }
    logLine("ATLAS %dx%d for %dx%d ready in %.1f ms (%s)", atlas.width(), atlas.height(), screenW, screenH,
            nowMs() - atlasStart, atlas.describe().c_str());

    ui::RendererEnv env = display->rendererEnv();
    env.width = screenW;
    env.height = screenH;
    env.atlas = &atlas; // the JUCE backends draw with the atlas's embedded Inter faces (env.fontData stays null)

    // 3./4. Probe and compare.
    const std::vector<std::string> order = settings["render"] == "auto" ? display->rendererOrder() : split(settings["render"]);
    const bool compare = settings["compare"] == "1";
    const int frames = compare ? std::max(30, std::atoi(settings["compare_frames"].c_str())) : 1;
    std::vector<std::string> candidates = order;
    if (compare)
        for (const char* extra : {"gl-fb", "gl-x11", "juce-gl", "soft", "juce"})
            if (ui::render::findRenderer(extra) != nullptr
                && std::find(candidates.begin(), candidates.end(), extra) == candidates.end())
                candidates.push_back(extra);
    std::vector<std::string> working;
    std::string probeFailures; // for the DIAG display line
    for (const auto& name : candidates) // GL first, all in children, before the parent opens X or JUCE's GUI
    {
        if (! ui::render::needsChildProbe(name))
            continue;
        std::string why;
        const bool ok = ui::render::probeInChild(
            name, env,
            [&](ui::Renderer& r) {
                logLine("COMPARE %s: %s", name.c_str(), r.describe().c_str());
                FrameStats stats;
                const bool drawn = measureFrames(r, atlas, frames, stats);
                if (frames > 1)
                    stats.print("compare", r.name());
                return drawn;
            },
            10000.0 + frames * 100.0, why, [&](const std::string& e) { logLine("PROBE %s: %s", name.c_str(), e.c_str()); });
        if (ok)
            working.push_back(name);
        else
        {
            logLine("PROBE %s unavailable: %s", name.c_str(), why.c_str());
            probeFailures += (probeFailures.empty() ? "" : "; ") + name + ": " + why;
        }
    }
    for (const auto& name : candidates)
    {
        if (ui::render::needsChildProbe(name))
            continue;
        auto r = ui::render::create(name, env, error);
        if (! r)
        {
            logLine("PROBE %s unavailable: %s", name.c_str(), error.c_str());
            probeFailures += (probeFailures.empty() ? "" : "; ") + name + ": " + error;
            continue;
        }
        working.push_back(name);
        if (compare)
        {
            logLine("COMPARE %s: %s", name.c_str(), r->describe().c_str());
            FrameStats stats;
            measureFrames(*r, atlas, frames, stats);
            stats.print("compare", r->name());
        }
    }

    // 5. Engine, audio, controls, run.
    // The engine binds every ParamSpec it knows by id; the FX bank routes the FX ids.
    engine::EngineSetup setup {[](std::string_view id) { return core::builtinParams().idOf(id).value_or(core::ParamId {}); }};
    for (const auto& spec : core::builtinParams().all())
        setup.paramIds.push_back(spec.id);
    setup.fxInternalIndex = core::fx::internalIndexTable(core::builtinParams());
    setup.testTone = settings["test_tone"] == "1"; // the 440 Hz tone on an empty track 1: smoke checks only
    auto engine = engine::createEngine(setup);
    juce::MessageManager::getInstance(); // JUCE's message thread = this thread (JUCE audio devices, juce-gl)
    std::vector<std::unique_ptr<platform::AudioDevice>> audioDevices;
    platform::AudioDevice* audio = nullptr;
    std::vector<std::string> diagAudio; // printed in the DIAG block below
    if (settings["audio"] == "1")
    {
        audioDevices = machine->audioDevices();
        for (auto& device : audioDevices)
        {
            std::string info;
            const std::string id(device->id());
            platform::AudioConfig audioConfig {};
            if (startPrefs.periodFrames > 0) // UTILITY › BUFFER SIZE (audio_period / <PREFIX>_AUDIO_PERIOD still win)
                audioConfig.periodFrames = startPrefs.periodFrames;
            if (device->start(audioConfig, *engine, info))
            {
                logLine("AUDIO ok %s", info.c_str());
                audio = device.get();
                const auto st = device->stats();
                diagAudio.push_back("audio device=" + id + " ok rate=" + std::to_string(st.sampleRate) + " period="
                                    + std::to_string(st.periodFrames) + " realtime=" + (st.realtime ? "yes" : "pending/no")
                                    + " ports " + diag::describePorts(*device));
                diagAudio.push_back("audio info " + info);
                break;
            }
            logLine("AUDIO %.*s unavailable: %s", static_cast<int>(device->id().size()), device->id().data(), info.c_str());
            diagAudio.push_back("audio device=" + id + " FAILED " + info);
        }
        if (audio == nullptr)
        {
            logLine("AUDIO FAILED no audio device started");
            diagAudio.push_back("audio FAILED no audio device started");
        }
    }
    else
        diagAudio.push_back("audio off (audio=0)");

    io::controllers::hub().setDataDir(machine->paths().dataDir); // <data dir>/Controllers (profiles, settings)
    auto sources = machine->controlSources({&engine->triggers()});
    for (const auto& entry : io::ControlSourceRegistry::builtin().all())
        if (auto source = entry.create(&engine->triggers()))
            sources.push_back(std::move(source));
    std::vector<std::string> diagInput;
    for (auto& source : sources)
    {
        std::string info;
        const bool ok = source->start(info);
        logLine("%s %s %s", upper(source->id()).c_str(), ok ? "ok" : "FAILED", info.c_str());
        diagInput.push_back("input " + std::string(source->id()) + (ok ? " ok " : " FAILED ") + info);
    }
    diagInput.push_back("input summary script=" + (settings["input_script"].empty() ? std::string("none") : settings["input_script"])
                        + " sources=" + std::to_string(sources.size()));
    auto lamps = machine->lamps(); // null when the platform has no lamps (host)
    diagInput.push_back(std::string("lamps ") + (lamps ? "present" : "none"));
    // UI-117: lamp output goes to every device that can show it — the platform's panel and every source that is also
    // a LampSink (MIDI controllers with lamp maps, the virtual panel).
    std::vector<io::LampSink*> lampSinks;
    if (lamps)
        lampSinks.push_back(lamps.get());
    for (auto& source : sources)
        if (auto* sink = dynamic_cast<io::LampSink*>(source.get()))
            lampSinks.push_back(sink);
    ui::virtualpanel::PanelHost panelHost(io::controllers::virtualPanel()); // ui/screens/virtualpanel

    std::unique_ptr<ui::Renderer> renderer;
    for (const auto& name : order)
        if (std::find(working.begin(), working.end(), name) != working.end() && (renderer = ui::render::create(name, env, error)))
            break;
    if (! renderer)
    {
        logLine("FATAL no renderer: %s", error.c_str());
        logLine("DIAG STOP reason=fatal uptime_s=%.1f", (nowMs() - runStart) / 1000.0);
        return 1;
    }
    logLine("RUN backend=%.*s %s", static_cast<int>(renderer->name().size()), renderer->name().data(),
            renderer->describe().c_str());
    // The rest of the DIAG block: display, audio, input.
    {
        std::string ok;
        for (const auto& w : working)
            ok += (ok.empty() ? "" : ",") + w;
        diagLine("display backend=" + std::string(renderer->name()) + " size=" + std::to_string(screenW) + "x"
                 + std::to_string(screenH) + (display->sizeKnown() ? "" : " (assumed)") + " order="
                 + settings["render"] + " working=" + (ok.empty() ? std::string("none") : ok) + " | "
                 + renderer->describe());
        if (! probeFailures.empty())
            diagLine("display probes failed: " + probeFailures);
        for (const auto& line : diagAudio)
            diagLine(line);
        for (const auto& line : diagInput)
            diagLine(line);
        diagLine("interval=" + std::to_string(diagIntervalS) + " s (FRAME / LOAD / AUDIO lines)");
        diagLine("END");
    }

    // 6. The document: model + CommandBus, sample library, project session, pads (app/Document.h).
    auto& bridge = engine->bridge();
    const double engineRate = audio != nullptr && audio->stats().sampleRate != 0
                                ? static_cast<double>(audio->stats().sampleRate)
                                : static_cast<double>(platform::AudioConfig {}.sampleRate);
    auto appLog = [](const std::string& line) { logLine("%s", line.c_str()); };
    auto document = std::make_unique<Document>(
        bridge, DocumentConfig {.dataDir = machine->paths().dataDir,
                                .device = machine->id() == "imx6",
                                .sampleRate = engineRate,
                                .devKit = settings["dev_kit"],
                                .devKitScenes = std::max(1, std::atoi(settings["dev_kit_scenes"].c_str())),
                                .internalDisk = settings["internal_disk"],
                                .log = appLog,
                                .audioDevice = audio != nullptr ? std::string(audio->id()) : std::string(),
                                .testTone = setup.testTone});
    auto root = ui::createRoot({core::builtinParams(), measureWith(atlas), &bridge, appLog, &document->values(),
                                document->summaries()});
    document->attach(*root);
    document->startup();
    // Every library is loaded now (the GL driver with the renderer): lock the code in RAM (platform::Tuning).
    if (lockCode)
        diagLine(app::tuning::describe(app::tuning::lockCode()));
    else
        diagLine("memlock off (mem_lock=0 or not this platform's default)");
    ui::DrawList dl;
    dl.reserve(2048, 16384);
    std::vector<io::ControlEvent> events;
    events.reserve(256);
    // GL backends block in the buffer swap until vsync, so the loop is paced by the display. A display without vsync
    // (Xvfb, a driver that ignores the swap interval) would let it spin: if the first 60 GL frames take less than
    // 0.6 s (> 100 fps), the CPU pacing below takes over and one PACE line says so.
    bool vsynced = renderer->name().rfind("gl-", 0) == 0 || renderer->name() == "juce-gl";
    double vsyncCheckStart = 0;
    const long runFrames = std::atol(settings["run_frames"].c_str());
    double loadMax = 0, loadP95Max = 0; // over every LOAD line, for DIAG STOP
    auto reportAudio = [&] {
        const auto s = audio != nullptr ? audio->stats() : platform::AudioStats {};
        const std::string_view id = audio != nullptr ? audio->id() : std::string_view("none");
        logLine("AUDIO alsa_periods=%llu xruns=%llu juce_callbacks=%llu engine_blocks=%llu device=%.*s",
                static_cast<unsigned long long>(id == "alsa-direct" ? s.periods : 0), static_cast<unsigned long long>(s.xruns),
                static_cast<unsigned long long>(id == "juce-alsa" ? s.periods : 0),
                static_cast<unsigned long long>(engine->blocksRendered()), static_cast<int>(id.size()), id.data());
        if (s.loopFrames >= 0) // the analog filter's send/return loop: is the return alive, does signal come back
            logLine("AUDIO filter_loop live=%d return_frames=%lld drops=%llu send_peak=%.4f return_peak=%.4f",
                    s.loopLive ? 1 : 0, static_cast<long long>(s.loopFrames),
                    static_cast<unsigned long long>(s.loopDrops), static_cast<double>(s.loopSendPeak),
                    static_cast<double>(s.loopReturnPeak));
        const auto load = engine->load(); // the engine's own per-block LoadMeter
        loadMax = std::max(loadMax, load.maxCycles);
        loadP95Max = std::max(loadP95Max, load.p95Cycles);
        logLine("LOAD cycles/block avg=%.0f p95=%.0f max=%.0f load=%.1f%% overruns=%llu periods=%llu xruns=%llu "
                "VmRSS=%s",
                load.avgCycles, load.p95Cycles, load.maxCycles, load.loadPercent,
                static_cast<unsigned long long>(load.overruns), static_cast<unsigned long long>(load.blocks),
                static_cast<unsigned long long>(s.xruns), residentKb().c_str());
    };
    FrameStats stats;
    std::uint64_t eventCount = 0, commands = 0;
    bool rendererLost = false;
    double frameMaxMs = 0;
    Screenshot screenshot(settings, [](const std::string& line) { logLine("%s", line.c_str()); });
    double next = nowMs(), lastReport = next;
    long frame = 0;
    // The display gives way to audio (app/UiThrottle.h): sampled every 250 ms; the thread table once the start-up's
    // threads exist (after 2 s), again with every `diag` command.
    UiThrottle throttle;
    double lastLoadSample = -1.0e9;
    std::uint64_t skippedFrames = 0;
    const double loopStart = next;
    bool threadsReported = false;
    while (! quitRequested && (runFrames <= 0 || frame < runFrames))
    {
        events.clear();
        for (auto& source : sources)
            source->poll(events);
        const double t0 = nowMs();
        eventCount += events.size();
        for (const auto& e : events)
            if (! panelHost.route(e, t0) && ! document->handle(e)) // a touch on the virtual panel is the panel's;
                root->handle(e, t0); // pads, pad modes, SHIFT, SCENE / PATTERN modes, step keys
        for (std::string cmd; io::takeAppCommand(cmd);) // the scripted input's `cmd …` (app/Document.h)
        {
            logLine("CMD %s", cmd.c_str());
            ++commands;
            if (cmd.rfind("mark", 0) == 0 && (cmd.size() == 4 || cmd[4] == ' ')) // a phase marker
                logLine("MARK %s t=%.1f s", cmd.size() > 5 ? cmd.c_str() + 5 : "", (nowMs() - runStart) / 1000.0);
            else if (cmd == "diag") // the periodic lines now
            {
                stats.print("run", renderer->name());
                reportAudio();
                stats = {};
                diag::threadLines(diagLine);
            }
            else if (cmd.rfind("diag-crash", 0) == 0) // check the CRASH report (deliberately)
            {
                logLine("DIAG crash requested (%s)", cmd.c_str());
                if (cmd.find("abort") != std::string::npos)
                    std::abort();
                ::raise(SIGSEGV);
            }
            else
                document->command(cmd);
        }
        document->frame(t0); // workers, project session, pool, preview, notifications
        UiThrottle::Sample loadSample;
        const UiThrottle::Sample* sampled = nullptr;
        if (t0 - lastLoadSample >= 250.0)
        {
            lastLoadSample = t0;
            const auto load = engine->load();
            loadSample.p95Fraction = load.p95Cycles / (engine::kBlockFrames * 1.0e9 / engineRate);
            loadSample.xruns = audio != nullptr ? audio->xruns() : 0;
            loadSample.overruns = load.overruns;
            sampled = &loadSample;
        }
        const bool draw = throttle.draw(t0, sampled);
        switch (throttle.takeChange())
        {
            case UiThrottle::Change::On:
                logLine("UI throttle on: 30 Hz (%s)", throttle.reason().c_str());
                break;
            case UiThrottle::Change::Off:
                logLine("UI throttle off after %.1f s (%llu frames skipped so far)", (t0 - throttle.sinceMs()) / 1000.0,
                        static_cast<unsigned long long>(skippedFrames));
                break;
            case UiThrottle::Change::None:
                break;
        }
        if (draw)
        {
            dl.clear();
            root->frame(dl, t0); // the Shell drains the engine's event queue itself (≤ 256 per frame)
            panelHost.frame(dl, ui::theme::palette(), t0); // the virtual panel drawer (hidden: nothing)
            const double built = nowMs() - t0;
            if (! lampSinks.empty()) // UI-150: the Shell's lamp model → every LampSink (on change + heartbeat in the sink)
                if (const auto* lampFrame = root->lamps())
                {
                    io::LampFrame shown = *lampFrame;
                    document->lamps(shown); // strip LEDs, MODE indicators, HOLD (UI-156/157)
                    for (auto& l : shown.lamps) // track colours → distinct saturated LED colours (ui::theme::ledColour)
                        l.rgb = ui::theme::ledColour(l.rgb);
                    for (auto* sink : lampSinks)
                        io::apply(shown, t0, document->lampBrightness(), *sink); // brightness (UI-159)
                }
            ui::FrameTiming timing;
            if (! renderer->render(dl, timing))
            {
                logLine("FATAL renderer lost");
                rendererLost = true;
                break;
            }
            ++frame;
            stats.add(built, timing);
            frameMaxMs = std::max(frameMaxMs, built + timing.rasterMs + timing.presentMs);
            screenshot.afterFrame(frame, *renderer);
        }
        else
        {
            ++skippedFrames;
            if (vsynced) // no buffer swap paces this frame: wait one display period here (CPU pacing does it below)
            {
                const double wait = t0 + 1000.0 / 60.0 - nowMs();
                if (wait > 0)
                    std::this_thread::sleep_for(std::chrono::microseconds(static_cast<long>(wait * 1000)));
            }
        }
        if (! threadsReported && t0 - loopStart >= 2000.0)
        {
            threadsReported = true;
            diag::threadLines(diagLine);
        }
        if (io::quitRequested()) // the window's close button, the scripted `quit` (io/controllers/AppRequests.h)
            break;
        if (io::standbyRequested()) // the panel's power key, the scripted `standby`: quit, then power off
            break;
        bridge.retirePending();
        const double now = nowMs();
        if (diagIntervalS > 0 && now - lastReport >= 1000.0 * static_cast<double>(diagIntervalS))
        {
            stats.print("run", renderer->name());
            reportAudio();
            stats = {};
            lastReport = now;
        }
        if (vsynced && frame == 1)
            vsyncCheckStart = nowMs();
        else if (vsynced && frame == 61 && nowMs() - vsyncCheckStart < 600.0)
        {
            logLine("PACE %.*s: swaps do not wait for vsync (%.0f fps), pacing to 60 Hz on the CPU",
                    static_cast<int>(renderer->name().size()), renderer->name().data(),
                    60000.0 / (nowMs() - vsyncCheckStart));
            vsynced = false;
            next = nowMs();
        }
        if (! vsynced) // CPU backends: pace to the 60 Hz UI refresh (threading.md)
        {
            next += 1000.0 / 60.0;
            const double wait = next - nowMs();
            if (wait > 0)
                std::this_thread::sleep_for(std::chrono::microseconds(static_cast<long>(wait * 1000)));
            else
                next = nowMs();
        }
    }
    const char* why = rendererLost ? "renderer-lost" : quitRequested ? "signal" : io::standbyRequested() ? "standby" : (runFrames > 0 && frame >= runFrames ? "run_frames" : "quit");
    if (! stats.total.v.empty())
    {
        stats.print("run", renderer->name());
        reportAudio();
    }
    {
        const auto st = audio != nullptr ? audio->stats() : platform::AudioStats {};
        const auto load = engine->load();
        const double upS = (nowMs() - runStart) / 1000.0;
        logLine("DIAG STOP reason=%s uptime_s=%.1f frames=%ld fps=%.1f frame_ms_max=%.2f events=%llu commands=%llu "
                "audio=%s periods=%llu xruns=%llu realtime=%s engine_blocks=%llu load_p95_max=%.0f load_max=%.0f "
                "overruns=%llu rss_kB=%ld peak_rss_kB=%ld",
                why, upS, frame, upS > 0 ? static_cast<double>(frame) / upS : 0.0, frameMaxMs,
                static_cast<unsigned long long>(eventCount), static_cast<unsigned long long>(commands),
                audio != nullptr ? std::string(audio->id()).c_str() : "none",
                static_cast<unsigned long long>(st.periods), static_cast<unsigned long long>(st.xruns),
                st.realtime ? "yes" : "no", static_cast<unsigned long long>(engine->blocksRendered()),
                std::max(loadP95Max, load.p95Cycles), std::max(loadMax, load.maxCycles),
                static_cast<unsigned long long>(load.overruns), diag::residentKb(), diag::peakResidentKb());
    }
    logLine("STOP");
    const bool standby = io::standbyRequested() && ! quitRequested && ! rendererLost;
    if (standby)
        machine->muteForStandby(); // the outputs go silent before anything stops (no click at the power cut)
    document->shutdown(standby);
    for (auto& device : audioDevices)
        device->stop();
    if (standby)
        powerOff(*machine); // returns only if the power stayed on
    for (auto& source : sources)
        source->stop();
    audioDevices.clear();
    renderer.reset();
    root.reset();
    document->detachPluginHost();
    engine.reset();   // its destroy functions give back the buffer references first …
    document.reset(); // … so the pool frees every buffer
    juce::DeletedAtShutdown::deleteAll(); // JUCE singletons (font cache, message manager) before static destruction
    juce::MessageManager::deleteInstance();
    return 0;
}
} // namespace app
