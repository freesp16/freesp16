// The engine frame under the real-time contract (NFR-020/021): createEngine() is the real Engine,
// process() marks the thread real-time and sets flush-to-zero itself, and a busy session — parameter ramps, swaps,
// transport, triggers from another thread, odd period sizes — allocates nothing on the audio thread.
#include "tests/unit/engine/EngineTestUtil.h"

#include "core/util/Realtime.h"
#include "tests/fakes/FakeAudioDevice.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <thread>

namespace
{
struct Blob
{
    float data[256] {};
};
void destroyBlob(const void* p) noexcept { delete static_cast<const Blob*>(p); }
} // namespace

TEST_CASE("engine: createEngine() returns the engine frame", "[engine]")
{
    auto e = engine::createEngine(enginetest::setup());
    REQUIRE(e != nullptr);
    CHECK(e->name() == "engine");
    CHECK(dynamic_cast<engine::Engine*>(e.get()) != nullptr);
}

TEST_CASE("engine: process() is real-time and flushes denormals on its own thread", "[engine][realtime]")
{
    auto e = enginetest::make();
    std::thread audio([&] {
        CHECK_FALSE(core::rt::isRealtimeThread());
        std::vector<float> l(256), r(256);
        float* out[2] = {l.data(), r.data()};
        e->process(out, 2, 256); // no ScopedRealtimeThread here: the engine opens its own
        CHECK_FALSE(core::rt::isRealtimeThread()); // restored after the call
        volatile float tiny = 1e-38f;            // a normal float; × 1e-3 is a denormal unless FTZ is on
        volatile float product = tiny * 1e-3f;
#if defined(__arm__) || defined(__aarch64__) || defined(__x86_64__)
        CHECK(product == 0.0f);
#else
        (void) product;
#endif
    });
    audio.join();
}

TEST_CASE("engine: a busy session allocates nothing on the audio thread", "[engine][realtime]")
{
    auto e = enginetest::make();
    e->acceptSwaps(engine::SwapSlot::Preview, &destroyBlob);
    fakes::FakeAudioDevice device;
    std::string info;
    platform::AudioConfig config;
    config.periodFrames = 333;
    REQUIRE(device.start(config, *e, info));
    auto& b = e->bridge();
    for (int round = 0; round < 50; ++round)
    {
        b.post(engine::ParamChange {enginetest::volumeId(), 0, static_cast<float>(round * 2 % 128), 100});
        b.post(engine::ObjectSwap {{engine::SwapSlot::Preview, 0, 0}, new Blob {}});
        b.post(engine::TransportCmd {round % 2 ? engine::TransportCmd::Stop : engine::TransportCmd::Play, 0});
        std::thread pad([&] { e->triggers().push({static_cast<std::uint8_t>(round % 16), 90, 60, 0}); });
        pad.join();
        REQUIRE(device.runPeriods(3));
        b.retirePending();
    }
    CHECK(e->triggersReceived() == 50);
    CHECK(e->blocksRendered() == (50 * 3 * 333 + 63) / 64);
#if APP_RT_CHECKS
    CHECK(core::rt::violationCount() == 0);
#endif
}

TEST_CASE("engine: OfflineDriver renders the tone at 48 and 96 kHz too", "[engine]")
{
    for (const double rate : {48000.0, 96000.0})
    {
        INFO("rate " << rate);
        auto e = enginetest::make(true, rate);
        const auto out = enginetest::render(*e, static_cast<int>(rate), 512);
        int crossings = 0;
        for (std::size_t i = 1; i < out.l.size(); ++i)
            crossings += (out.l[i - 1] < 0) != (out.l[i] < 0);
        CHECK(crossings / 2 == Catch::Approx(440).margin(2));
        CHECK(enginetest::peak(out.l) == Catch::Approx(0.25).epsilon(0.02));
    }
}
