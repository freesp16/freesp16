// Ports, threads and hot-plug (MID-001/002/003, NFR-013): the scripted virtual port end to end through the port
// thread (parse + stamp), THRU forwarding on the sender, reconnect by name, and an engine rendering under the
// real-time contract on another thread the whole time (hot-plug never touches the audio thread: no allocation, no
// lock, blocks keep coming).
#include "tests/unit/midi/MidiTestUtil.h"

#include "io/midi/MidiService.h"
#include "io/midi/VirtualPort.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <set>
#include <chrono>
#include <thread>

namespace
{
void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
} // namespace

TEST_CASE("midi.service: virtual port end to end, THRU forwarding, hot-plug by name with the engine running", "[midi]")
{
    auto e = miditest::make();
    auto& hub = e->midi().hub();
    std::atomic<bool> quit {false};
    std::atomic<std::uint64_t> periods {0};
    std::thread audio([&] {
        std::vector<float> l(256), r(256);
        float* out[2] = {l.data(), r.data()};
        while (! quit.load())
        {
            engine::OfflineDriver drv;
            drv.periodFrames = 256;
            drv.run(*e, 256, out); // real-time contract: an allocation here aborts the test
            periods.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    });

    auto script = io::midi::parseVirtualScript("50 90 05 64\n100 80 05 00\n150 B0 07 7F\n");
    REQUIRE(script.errors.empty());
    auto provider = std::make_unique<io::midi::VirtualProvider>(std::move(script));
    auto* virt = provider.get();
    std::vector<std::unique_ptr<io::midi::PortProvider>> providers;
    providers.push_back(std::move(provider));
    io::midi::MidiService service(hub, std::move(providers));
    hub.setPortMode(0, 3); // THRU A: the output forwards input A
    std::string info;
    REQUIRE(service.start(info));
    INFO(info);
    CHECK(service.inputs()[0].connected);
    CHECK(service.outputs()[0].connected);
    CHECK((hub.outputMask() & 1u) == 1u);
    CHECK((hub.inputMask() & 1u) == 1u);
    for (int i = 0; i < 100 && service.messagesIn() < 3; ++i)
        sleepMs(10);
    CHECK(service.messagesIn() == 3);
    for (int i = 0; i < 100 && virt->received().size() < 3; ++i)
        sleepMs(10);
    const auto fwd = virt->received(); // THRU A: the three messages, as received
    REQUIRE(fwd.size() == 3);
    CHECK((fwd[0].bytes[0] == 0x90 && fwd[0].bytes[1] == 5 && fwd[0].bytes[2] == 0x64));
    CHECK((fwd[2].bytes[0] == 0xB0 && fwd[2].bytes[1] == 7));
    const auto gap = static_cast<double>(fwd[1].atNs - fwd[0].atNs) / 1.0e6;
    CHECK((gap > 40.0 && gap < 60.0)); // replayed on time (50 ms apart)
    CHECK(service.sender().stats().forwarded == 3);

    // Hot-plug: the port vanishes, then comes back on the same slot (by name).
    const auto gen = hub.generation(0);
    for (int round = 0; round < 5; ++round)
    {
        virt->setPresent(false);
        service.refresh();
        CHECK_FALSE(service.inputs()[0].connected);
        CHECK_FALSE(service.outputs()[0].connected);
        CHECK((hub.outputMask() & 1u) == 0u);
        virt->setPresent(true);
        service.refresh();
        CHECK(service.inputs()[0].connected);
        CHECK(service.outputs()[0].connected);
        CHECK(service.inputs()[0].name == "VIRTUAL");
        CHECK(service.inputs()[1].name.empty()); // no new slot for a known name
    }
    CHECK(hub.generation(0) == gen + 10); // each disconnect / connect bumps it (the hanging-note guard)
    CHECK(service.reconnects() == 12);
    const auto before = periods.load();
    sleepMs(50);
    service.stop();
    quit = true;
    audio.join();
    CHECK(periods.load() > before);
}

TEST_CASE("midi.providers: the registered providers list their ports (ALSA raw MIDI on Linux builds)", "[midi]")
{
    const auto entries = io::midi::portProviders();
#if defined(__linux__)
    REQUIRE(std::any_of(entries.begin(), entries.end(), [](const auto& e) { return e.id == "alsa-rawmidi"; }));
#endif
    for (const auto& entry : entries)
    {
        auto provider = entry.create();
        REQUIRE(provider);
        const auto ports = provider->list(); // enumeration only: no port is opened here
        std::set<std::string> names;
        for (const auto& p : ports)
        {
            std::printf("midi.providers: %.*s: %s \"%s\"%s%s\n", static_cast<int>(entry.id.size()), entry.id.data(),
                        p.id.c_str(), p.name.c_str(), p.input ? " in" : "", p.output ? " out" : "");
            CHECK(names.insert(p.name).second); // the reconnect key is unique
            CHECK((p.input || p.output));
        }
    }
}
