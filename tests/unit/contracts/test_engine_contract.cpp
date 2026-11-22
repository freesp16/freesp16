// Engine contract (threading.md; NFR-020/021; UI-118): the messages are
// trivially copyable, the engine createEngine() returns runs as an AudioProcessor under the real-time allocation hook
// for any frame count, renders the Main pair only, hears bridge posts and live triggers, publishes meters.
#include "engine/EngineApi.h"

#include "core/params/ParamRegistry.h"
#include "core/util/Realtime.h"
#include "tests/fakes/FakeAudioDevice.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <thread>

namespace
{
std::unique_ptr<engine::AudioEngine> makeEngine()
{
    return engine::createEngine({[](std::string_view id) { return core::builtinParams().idOf(id).value_or(core::ParamId {}); }});
}

float peak(const std::vector<float>& v, std::size_t from = 0)
{
    float p = 0;
    for (std::size_t i = from; i < v.size(); ++i)
        p = std::max(p, std::fabs(v[i]));
    return p;
}
} // namespace

TEST_CASE("engine messages are plain data", "[contracts][engine]")
{
    STATIC_REQUIRE(std::is_trivially_copyable_v<engine::EngineMessage>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<engine::TriggerNow>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<engine::PlayheadFeed>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<engine::MeterFeed>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<engine::EngineEvent>);
    STATIC_REQUIRE(engine::kBlockFrames == 64);
    STATIC_REQUIRE(sizeof(engine::EngineMessage) <= 32); // a 4096-entry FIFO stays small
}

TEST_CASE("createEngine: the AudioProcessor renders the test tone on Main under the RT hook", "[contracts][engine][realtime]")
{
    auto engine = makeEngine();
    REQUIRE(engine != nullptr);
    CHECK_FALSE(engine->name().empty());
    fakes::FakeAudioDevice device;
    std::string info;
    platform::AudioConfig config;
    config.periodFrames = 100; // not a multiple of 64: exercises the block adapter
    REQUIRE(device.start(config, *engine, info));
    REQUIRE(device.runPeriods(441)); // 1 s
    const auto& l = device.mainLeft();
    REQUIRE(l.size() == 44100);
    CHECK(peak(l) == Catch::Approx(0.25).epsilon(0.02)); // -12 dBFS
    int crossings = 0;
    for (std::size_t i = 1; i < l.size(); ++i)
        crossings += (l[i - 1] < 0) != (l[i] < 0);
    CHECK(crossings / 2 == Catch::Approx(440).margin(2));
    CHECK(engine->blocksRendered() == (44100 + 63) / 64);
    CHECK(engine->bridge().meters().masterPeak[0] > 0.2f);
    CHECK(engine->bridge().playhead().blockIndex == engine->blocksRendered());
#if APP_RT_CHECKS
    CHECK(core::rt::violationCount() == 0);
#endif
}

TEST_CASE("createEngine: bridge posts and live triggers reach the audio thread", "[contracts][engine]")
{
    auto engine = makeEngine();
    fakes::FakeAudioDevice device;
    std::string info;
    REQUIRE(device.start(platform::AudioConfig {}, *engine, info));
    auto& bridge = engine->bridge();
    const auto volume = core::builtinParams().idOf("track.volume");
    REQUIRE(volume.has_value());
    // track.volume of track 1 → 0 silences the tone after the fader's 100-sample ramp (MIX-002)
    REQUIRE(bridge.post(engine::ParamChange {*volume, 0, 0.0f, 64}));
    REQUIRE(device.runPeriods(8));
    CHECK(peak(device.mainLeft(), 100) == 0.0f);
    // an unknown parameter or an object swap of a kind no consumer accepts is refused (the sender keeps ownership);
    // TrackSample / Preview are taken by the voices (voice::acceptVoiceSwaps), Pattern by the sequencer stage, LiveTake
    // by the capture hub, SliceMap by the slice tracks: every kind has a consumer, so a kind value outside the enum
    // stands for "no consumer"
    CHECK_FALSE(bridge.post(engine::ParamChange {core::ParamId {999}, 0, 1.0f, 0}));
    int object = 0;
    const auto noKind = static_cast<engine::SwapSlot::Kind>(0xFF); // far outside the enum (kinds get appended)
    CHECK_FALSE(bridge.post(engine::ObjectSwap {{noKind, 0, 0}, &object}));
    CHECK(bridge.post(engine::TransportCmd {engine::TransportCmd::Play, 0}));

    // A pad hit from another thread (UI-118) is heard in the next period (the test ping plays on track 1: fader back up;
    // the tone alone peaks at 0.25).
    REQUIRE(bridge.post(engine::ParamChange {*volume, 0, 100.0f, 64}));
    REQUIRE(device.runPeriods(2));
    device.clearCapture();
    std::thread reader([&] { CHECK(engine->triggers().push({0, 127, 60, 0})); });
    reader.join();
    REQUIRE(device.runPeriods(4));
    CHECK(peak(device.mainLeft()) > 0.3f);
    engine::EngineEvent e {};
    CHECK_FALSE(bridge.popEvent(e));
    bridge.retirePending();
}

TEST_CASE("AudioIO: absent ports are skipped, other ports silent", "[contracts][engine]") // + the Phones port
{
    auto engine = makeEngine();
    engine->prepare(44100.0, 256);
    std::vector<float> main(512, 1.0f), phones(512, 1.0f);
    std::array<float*, 2 * static_cast<int>(engine::OutPort::Count)> out {};
    out[0] = main.data();
    out[1] = main.data() + 256;
    out[2 * static_cast<int>(engine::OutPort::Phones)] = phones.data();
    out[2 * static_cast<int>(engine::OutPort::Phones) + 1] = phones.data() + 256;
    {
        core::rt::ScopedRealtimeThread realtime;
        engine->process({out.data(), static_cast<int>(engine::OutPort::Count), nullptr, 0, 256, 0});
    }
    CHECK(peak(main) > 0.2f);
    // The Phones port plays the headphone cue (MASTER by default) at HEADPHONES 100 = master × 0.5
    // (MIX-040/041); the absent pairs (OUT 3-4 … 7-8, FilterSend) are not touched.
    CHECK(peak(phones) == peak(main) * 0.5f);
    // no Main pair at all: still renders (time runs), writes nothing
    std::array<float*, 2 * static_cast<int>(engine::OutPort::Count)> none {};
    const auto before = engine->blocksRendered();
    {
        core::rt::ScopedRealtimeThread realtime;
        engine->process({none.data(), static_cast<int>(engine::OutPort::Count), nullptr, 0, 128, 0});
    }
    CHECK(engine->blocksRendered() > before);
}
