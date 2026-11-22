// NFR-013: MIDI ports (here: fake control sources, the io contract every MIDI reader implements) opened, polled and
// closed on the message thread while the offline engine renders on its own real-time thread never cost a block: the
// engine renders exactly periods × (period / 64) blocks, and pad hits from the "ports" reach it through the
// multi-producer TriggerSink.
#include "ProjectTestSupport.h"

#include "engine/EngineApi.h"
#include "tests/fakes/FakeAudioDevice.h"
#include "tests/fakes/FakeControlSource.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>

TEST_CASE("project.hotplug: opening and closing MIDI ports while the engine runs never misses a block", "[project]")
{
    auto engine = engine::createEngine();
    fakes::FakeAudioDevice device;
    std::string info;
    platform::AudioConfig config;
    config.periodFrames = 256;
    REQUIRE(device.start(config, *engine, info));
    const auto blocksBefore = engine->blocksRendered();

    constexpr int kPeriods = 2000;
    std::atomic<bool> running {true};
    std::atomic<int> rendered {0};
    std::thread audio(
        [&]
        {
            for (int p = 0; p < kPeriods; ++p)
            {
                device.runPeriods(1);
                if (p % 64 == 63)
                    device.clearCapture();
                rendered.fetch_add(1, std::memory_order_relaxed);
            }
            running = false;
        });

    int opened = 0, hits = 0;
    std::vector<io::ControlEvent> events;
    while (running.load())
    {
        // A port appears, delivers a burst, disappears (hot-plug), on the message thread.
        auto port = std::make_unique<fakes::FakeControlSource>(&engine->triggers(), "midi-" + std::to_string(opened));
        REQUIRE(port->start(info));
        ++opened;
        for (int k = 0; k < 4; ++k, ++hits)
            port->emit({io::ControlEvent::Kind::Press, io::Control::Pad1, 100, 2, 0.0});
        events.clear();
        port->poll(events);
        port->stop();
        port.reset();
        std::this_thread::yield();
    }
    audio.join();
    CHECK(rendered.load() == kPeriods);
    CHECK(engine->blocksRendered() - blocksBefore == static_cast<std::uint64_t>(kPeriods) * (256 / engine::kBlockFrames));
    CHECK(opened > 0);
    INFO("ports opened and closed: " << opened << ", pad hits: " << hits);
}
