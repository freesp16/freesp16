// The platform layer: everything that knows the physical machine — the audio codec, the panel and strip, the display
// handles, the file-system paths — behind one interface per concern, one sub-folder per platform (platform/imx6
// = the reference hardware, platform/host = macOS/Linux; a Raspberry Pi would be platform/rpi). The composition root
// (app/) calls platform::select("auto") and then only these interfaces; core, engine, services, io and ui never include
// platform/** (tools/check-deps.py).
//
// Real-time: AudioDevice owns the only real-time thread a platform creates. Per period it calls
// engine::AudioProcessor::process once and otherwise only converts formats into pre-allocated buffers (NFR-020/021);
// no virtual call per sample. Spec: 01-architecture/threading.md (audio thread), audio-engine.md (outputs),
// 03-ui/control-surface.md (UI-100…118 input, UI-150… lamps).
#pragma once

#include "engine/EngineApi.h"
#include "io/controllers/ControlSurface.h"
#include "ui/render/RendererEnv.h"

#include "core/util/Registrar.h"

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace platform
{
enum class AudioPort : std::uint8_t { Main, Out34, Out56, Out78, Phones, FilterSend, Count };   // logical stereo ports
static_assert(static_cast<int>(AudioPort::Count) == static_cast<int>(engine::OutPort::Count)
                  && static_cast<int>(AudioPort::Main) == static_cast<int>(engine::OutPort::Main)
                  && static_cast<int>(AudioPort::FilterSend) == static_cast<int>(engine::OutPort::FilterSend),
              "platform::AudioPort and engine::OutPort index the same AudioIO::out pairs");
struct PortInfo { AudioPort port; std::int8_t left, right; };          // physical interleaved channels, -1 = absent
struct AudioConfig { unsigned sampleRate = 44100, periodFrames = 256, periods = 3; }; // priority: Role::Audio
struct AudioStats
{
    std::uint64_t periods = 0, xruns = 0;
    unsigned sampleRate = 0, periodFrames = 0;
    bool realtime = false;
    // A send/return loop (imx6: the analog filter), when the device has one: return frames read so far (−1 = no loop),
    // periods it fell short, whether the return is live, and the send / return peaks since the last stats() (linear).
    std::int64_t loopFrames = -1;
    std::uint64_t loopDrops = 0;
    bool loopLive = false;
    float loopSendPeak = 0.0f, loopReturnPeak = 0.0f;
};
class AudioDevice {                                    // the codec; owns the only RT thread a platform creates
public:
    virtual ~AudioDevice() = default;
    virtual std::string_view id() const noexcept = 0;  // "alsa-direct", "juce-alsa", "offline", ...
    virtual std::span<const PortInfo> ports() const noexcept = 0;
    // Calls processor.prepare(rate, maxFrames) on this (the message) thread, then starts the device thread, which
    // calls processor.process(AudioIO) once per period: out pairs in AudioPort order, null = port absent.
    virtual bool start(const AudioConfig&, engine::AudioProcessor&, std::string& info) = 0;   // message thread
    virtual void stop() = 0;
    virtual AudioStats stats() const noexcept = 0;
    // The xrun count alone, without stats()'s side effects (it resets the loop peaks): polled by the UI throttle.
    virtual std::uint64_t xruns() const noexcept { return stats().xruns; }
};
struct Paths { std::string dataDir, settingsFile, cacheDir; std::vector<std::string> usbMounts; };
class DisplayDevice {
public:
    virtual ~DisplayDevice() = default;
    virtual int width() const noexcept = 0;  virtual int height() const noexcept = 0;   // native pixels (800 x 480)
    virtual std::vector<std::string> rendererOrder() const = 0;                            // "auto" expands to this
    virtual ui::RendererEnv rendererEnv() = 0;           // native display/window factories + GL loader for ui/render
    virtual void setBacklight(float /*0..1*/) {}
    // False when the size could not be read from the display and width()/height() are the 800 × 480 default.
    virtual bool sizeKnown() const noexcept { return true; }
};
struct ControlEnv { engine::TriggerSink* triggers; };  // UI-118: pad hits go straight to the engine

// How the machine runs the app's threads (threading.md "Threads"; the app applies it before any thread starts):
//   threadMap  the default core / priority map of the thread roles (core/util/ThreadRole.h syntax; empty = every role
//              keeps its inherited cores); the setting thread_map goes over it ("none" = no map at all)
//   lockCode   lock the executable's and the libraries' code in RAM after start-up, so the audio thread never waits for
//              a page read back from the medium the program runs from (setting mem_lock=0/1)
//   governor   the CPU frequency governor while the app runs ("" = leave it; setting cpu_governor=keep|<name>)
//   rtRuntime  kernel.sched_rt_runtime_us while the app runs ("" = leave it; setting rt_runtime_us=keep|<µs>|-1): with
//              −1 (no limit) one real-time thread that never sleeps keeps its core forever; the kernel's default
//              950000 leaves the normal threads 5 % of every second on each core
struct Tuning { std::string threadMap; bool lockCode = false; std::string governor; std::string rtRuntime; };

// The settings the app read (key=value file at Paths::settingsFile + <PREFIX>_<KEY> environment overrides).
using Settings = std::map<std::string, std::string, std::less<>>;

class Platform {
public:
    virtual ~Platform() = default;
    virtual std::string_view id() const noexcept = 0;   // "imx6", "host"
    virtual std::string describe() const = 0;           // one log line
    virtual bool detect() const = 0;                    // true on matching hardware
    virtual Paths paths() const = 0;
    virtual std::vector<std::unique_ptr<AudioDevice>> audioDevices() = 0;   // in fallback order
    virtual std::vector<std::unique_ptr<io::ControlSource>> controlSources(const ControlEnv&) = 0;
    virtual std::unique_ptr<io::LampSink> lamps() = 0;  // null = no lamps
    virtual std::unique_ptr<DisplayDevice> display(std::string& error) = 0;
    // Called once with the settings before the factories above (e.g. imx6 audio_device = the codec PCM name).
    virtual void configure(const Settings&) {}
    virtual Tuning tuning() const { return {}; } // the defaults; the settings go over them
    // The power key was pressed: mute the analog outputs at once, before the app stops its audio (imx6: HardMute).
    virtual void muteForStandby() {}
    // Power off (the app has shut down cleanly and synced): hand the machine to its power controller. True = the
    // power is going (imx6: the panel's standby flag, after which the unit's M1 cuts the power); the caller keeps
    // the control sources running and waits. False = no power controller (host, or no panel link): just quit.
    virtual bool enterStandby(std::string& /*info*/) { return false; }
};
using PlatformFactory = std::unique_ptr<Platform> (*)();
void registerPlatform(std::string_view id, PlatformFactory);
std::unique_ptr<Platform> select(std::string_view wanted /* "auto" | id */, std::string& log);
std::vector<std::string_view> registeredPlatforms(); // ids, sorted
} // namespace platform

// APP_REGISTER_PLATFORM("imx6", &factory): static registrar (platform/<id>/<Id>Platform.cpp).
#define APP_REGISTER_PLATFORM(id, factory) APP_STATIC_REGISTRAR(::platform::registerPlatform(id, factory))
