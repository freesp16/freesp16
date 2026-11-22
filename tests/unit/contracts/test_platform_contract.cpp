// Platform contract: every registered platform that can run on
// the host, plus FakePlatform — audio device start/stop with the processor called on its thread, port map
// consistent, control sources poll without blocking, the display hands the renderer registry a working env, Paths
// absolute, select() rules. The linux platform runs headless here: its file audio device, its input source
// before any window exists, and its display's size and renderer order; opening the X11 window is the job of
// scripts/host-smoke.sh (under Xvfb).
#include "platform/Platform.h"
#include "ui/render/RendererRegistry.h"

#include "tests/fakes/FakePlatform.h"
#include "ui/render/GlyphAtlas.h"


#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <thread>

namespace
{
// Counts periods; checks it runs on a real-time-marked thread and never on the test thread after start().
class CountingProcessor final : public engine::AudioProcessor
{
public:
    void prepare(double sampleRate, int maxDeviceFrames) override
    {
        rate = sampleRate;
        maxFrames = maxDeviceFrames;
    }
    void process(const engine::AudioIO& io) noexcept override
    {
        frames.store(io.frames);
        if (io.numOutPorts > 0 && io.out[0] != nullptr)
            for (int i = 0; i < io.frames; ++i)
                io.out[0][i] = io.out[1][i] = 0.0f;
        calls.fetch_add(1);
    }
    double rate = 0;
    int maxFrames = 0;
    std::atomic<int> calls {0}, frames {0};
};

// `render`: create the CPU renderers of the display's order and draw (false: the platform's renderers need a display
// server the test does not have, e.g. linux's X11 window).
void checkPlatform(platform::Platform& p, bool render = true)
{
    INFO("platform " << p.id());
    CHECK_FALSE(p.id().empty());
    CHECK_FALSE(p.describe().empty());

    const auto paths = p.paths();
    for (const auto* path : {&paths.dataDir, &paths.settingsFile, &paths.cacheDir})
    {
        INFO(*path);
        CHECK((! path->empty() && path->front() == '/'));
    }
    for (const auto& m : paths.usbMounts)
        CHECK((! m.empty() && m.front() == '/'));

    auto devices = p.audioDevices();
    REQUIRE_FALSE(devices.empty());
    for (auto& d : devices)
    {
        INFO("audio device " << d->id());
        CHECK_FALSE(d->id().empty());
        const auto ports = d->ports();
        REQUIRE_FALSE(ports.empty());
        std::set<int> channels, logical;
        bool main = false;
        for (const auto& port : ports)
        {
            CHECK(static_cast<int>(port.port) < static_cast<int>(platform::AudioPort::Count));
            CHECK(logical.insert(static_cast<int>(port.port)).second); // one entry per logical port
            CHECK(((port.left < 0) == (port.right < 0)));               // both channels present or both absent
            if (port.left >= 0)
            {
                CHECK(port.left != port.right);
                CHECK(channels.insert(port.left).second);
                CHECK(channels.insert(port.right).second);
            }
            main = main || (port.port == platform::AudioPort::Main && port.left >= 0);
        }
        CHECK(main);
    }

    // The first device must start, call the processor and stop (the offline device runs its own paced thread).
    CountingProcessor processor;
    std::string info;
    auto& device = *devices.front();
    REQUIRE(device.start(platform::AudioConfig {}, processor, info));
    CHECK(processor.rate == 44100.0);
    CHECK(processor.maxFrames >= 64);
    if (auto* fake = dynamic_cast<fakes::FakeAudioDevice*>(&device))
        REQUIRE(fake->runPeriods(4));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (processor.calls.load() < 3 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    device.stop();
    CHECK(processor.calls.load() >= 3);
    CHECK(processor.frames.load() > 0);
    CHECK(device.stats().periods >= 3);
    CHECK(device.stats().xruns == 0);
    const int after = processor.calls.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(processor.calls.load() == after); // stopped means stopped

    // Control sources: start may fail without hardware, poll never blocks.
    struct NullSink final : engine::TriggerSink
    {
        bool push(const engine::TriggerNow&) noexcept override { return true; }
    } sink;
    for (auto& source : p.controlSources({&sink}))
    {
        INFO("control source " << source->id());
        CHECK_FALSE(source->id().empty());
        std::vector<io::ControlEvent> events;
        const auto t0 = std::chrono::steady_clock::now();
        source->poll(events);
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(5));
        source->stop();
    }
    (void) p.lamps(); // null = no lamps: allowed

    std::string error;
    auto display = p.display(error);
    REQUIRE(display != nullptr);
    CHECK(display->width() > 0);
    CHECK(display->height() > 0);
    const auto order = display->rendererOrder();
    REQUIRE_FALSE(order.empty());
    for (const auto& name : order)
    {
        INFO("renderer " << name);
        CHECK(ui::render::findRenderer(name) != nullptr);
    }
    if (! render)
        return;
    // The CPU part of the order renders through the platform's env (GL backends are probed on the device only).
    std::vector<std::string> cpu;
    for (const auto& name : order)
        if (! ui::render::needsChildProbe(name))
            cpu.push_back(name);
    REQUIRE_FALSE(cpu.empty());
    auto env = display->rendererEnv();
    ui::GlyphAtlas atlas;
    REQUIRE(atlas.build(ui::CanvasTransform::fit(display->width(), display->height()).scale, error));
    env.atlas = &atlas;
    std::string log;
    auto r = ui::render::createFirstWorking(cpu, env, log);
    INFO(log);
    REQUIRE(r != nullptr);
    CHECK(r->name() == cpu.front());
    ui::DrawList dl;
    dl.fill({0, 0, 1280, 768}, 0xFF102030u);
    ui::FrameTiming t;
    CHECK(r->render(dl, t));
    std::vector<std::uint32_t> px;
    if (r->readPixels(px))
        CHECK(px[px.size() / 2] == 0x102030u);
}
} // namespace

TEST_CASE("platform contract: every host-runnable platform and the fake", "[contracts][platform]")
{
    const auto ids = platform::registeredPlatforms();
    REQUIRE(std::find(ids.begin(), ids.end(), "host") != ids.end());
    std::string log;
    auto host = platform::select("host", log);
    REQUIRE(host != nullptr);
    CHECK(host->id() == "host");
    checkPlatform(*host);

    fakes::FakePlatform fake;
    checkPlatform(fake);
}

TEST_CASE("platform contract: linux, headless (file audio, no window yet)", "[contracts][platform]")
{
    const auto ids = platform::registeredPlatforms();
    if (std::find(ids.begin(), ids.end(), "linux") == ids.end())
        SKIP("the linux platform is built on Linux hosts only");
    std::string log;
    auto p = platform::select("linux", log);
    REQUIRE(p != nullptr);
    REQUIRE(p->id() == "linux");
    const auto wav = (std::filesystem::temp_directory_path() / "platform_contract_linux.wav").string();
    p->configure({{"audio_device", "file:" + wav}, {"scale", "2"}});
    {
        auto devices = p->audioDevices();
        REQUIRE(devices.size() == 2);
        CHECK(devices[0]->id() == "file");
        CHECK(devices[1]->id() == "offline"); // the fallback when nothing else starts
    }
    checkPlatform(*p, false);
    CHECK(std::filesystem::file_size(wav) >= 44 + 3 * 256 * 4); // header + the periods the check ran
    std::filesystem::remove(wav);
    std::string error;
    auto display = p->display(error);
    REQUIRE(display != nullptr);
    CHECK(display->width() == 1600);
    CHECK(display->height() == 960);
    CHECK(display->sizeKnown());
    CHECK(display->rendererOrder() == std::vector<std::string> {"soft"}); // gl-x11 only with gldispatch (the app)
    CHECK(p->lamps() == nullptr);                                         // lamp_log off by default
    CHECK(p->describe().find("file " + wav) != std::string::npos);

    // ALSA: the app's built-in imx6 default means `default` on the desktop; other names pass through. Not started here.
    p->configure({{"audio_device", "hw:ak4458audio,0"}});
    CHECK(p->describe().find("alsa default (MAIN = ch 0/1 of 2)") != std::string::npos);
    p->configure({{"audio_device", "pipewire"}, {"lamp_log", "1"}});
    auto devices = p->audioDevices();
    REQUIRE(devices.size() == 2);
    CHECK(devices[0]->id() == "alsa-direct");
    REQUIRE(devices[0]->ports().size() == 6);
    CHECK(devices[0]->ports()[0].left == 0);
    CHECK(devices[0]->ports()[0].right == 1);
    for (std::size_t i = 1; i < devices[0]->ports().size(); ++i)
        CHECK(devices[0]->ports()[i].left == -1);
    CHECK(p->lamps() != nullptr);
}

TEST_CASE("platform::select: auto, named, unknown, detection order", "[contracts][platform]")
{
    // Without DISPLAY no desktop platform is detected (linux needs one); restored at the end.
    const char* had = std::getenv("DISPLAY");
    const std::string savedDisplay = had != nullptr ? had : "";
    unsetenv("DISPLAY");
    std::string log;
    auto p = platform::select("auto", log);
    REQUIRE(p != nullptr);
    CHECK(p->id() == "host"); // no hardware on a build host
    CHECK(log.find("host selected") != std::string::npos);

    log.clear();
    p = platform::select("no-such-platform", log);
    REQUIRE(p != nullptr);
    CHECK(p->id() == "host");
    CHECK(log.find("not built in") != std::string::npos);

    platform::registerPlatform("fake", &fakes::makeFakePlatform);
    fakes::FakePlatform::detected = false;
    log.clear();
    CHECK(platform::select("auto", log)->id() == "host");
    fakes::FakePlatform::detected = true;
    log.clear();
    CHECK(platform::select("auto", log)->id() == "fake"); // detected hardware wins over the host fallback
    fakes::FakePlatform::detected = false;
    log.clear();
    CHECK(platform::select("fake", log)->id() == "fake");

    const auto ids = platform::registeredPlatforms();
    if (std::find(ids.begin(), ids.end(), "linux") != ids.end())
    {
        setenv("DISPLAY", ":77", 1); // detect() only reads the variable, nothing connects
        log.clear();
        CHECK(platform::select("auto", log)->id() == "linux"); // a Linux desktop (not the reference hardware)
        CHECK(log.find("linux selected (auto: detected") != std::string::npos);
    }
    if (had != nullptr)
        setenv("DISPLAY", savedDisplay.c_str(), 1);
    else
        unsetenv("DISPLAY");
}

TEST_CASE("platform ports and engine ports agree", "[contracts][platform]")
{
    STATIC_REQUIRE(static_cast<int>(platform::AudioPort::Count) == static_cast<int>(engine::OutPort::Count));
    STATIC_REQUIRE(static_cast<int>(platform::AudioPort::Phones) == static_cast<int>(engine::OutPort::Phones));
}
