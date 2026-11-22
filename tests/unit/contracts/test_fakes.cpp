// Every fake of tests/fakes/ does what the tests that use it rely on (every fake has a test).
#include "tests/fakes/FakeAudioDevice.h"
#include "tests/fakes/FakeClock.h"
#include "tests/fakes/FakeControlSource.h"
#include "tests/fakes/FakeEffect.h"
#include "tests/fakes/FakeEngineBridge.h"
#include "tests/fakes/FakePlatform.h"
#include "tests/fakes/FakeStretcher.h"
#include "tests/fakes/OffscreenRenderer.h"

#include "core/params/ParamRegistry.h"
#include "core/util/Realtime.h"
#include "ui/shell/Root.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

TEST_CASE("FakeClock advances only when told", "[contracts][fakes]")
{
    fakes::FakeClock clock(100);
    const core::Clock& c = clock;
    CHECK(c.nowMs() == 100);
    clock.advance(16.5);
    CHECK(c.nowMs() == 116.5);
    clock.set(0);
    CHECK(c.nowMs() == 0);
}

TEST_CASE("FakeEngineBridge records posts and serves feeds and events", "[contracts][fakes]")
{
    fakes::FakeEngineBridge bridge;
    engine::EngineBridge& b = bridge;
    CHECK(b.post(engine::TriggerNow {2, 100, 60, 2}));
    bridge.accept = false;
    CHECK_FALSE(b.post(engine::TransportCmd {engine::TransportCmd::Stop, 0}));
    REQUIRE(bridge.posted.size() == 1);
    CHECK(std::get<engine::TriggerNow>(bridge.posted[0]).track == 2);
    bridge.meterFeed.masterPeak = {0.5f, 0.25f};
    CHECK(b.meters().masterPeak[1] == 0.25f);
    bridge.events.push_back({engine::EngineEvent::Kind::RecordedHit, 3, 7, 100, -2});
    engine::EngineEvent e {};
    REQUIRE(b.popEvent(e));
    CHECK((e.kind == engine::EngineEvent::Kind::RecordedHit && e.step == 7 && e.offset24 == -2));
    CHECK_FALSE(b.popEvent(e));
    b.retirePending();
    CHECK(bridge.retireCalls == 1);
}

TEST_CASE("FakeAudioDevice drives a processor offline on the calling thread", "[contracts][fakes][realtime]")
{
    struct Ramp final : engine::AudioProcessor
    {
        float next = 0;
        void prepare(double, int) override {}
        void process(const engine::AudioIO& io) noexcept override
        {
            for (int i = 0; i < io.frames; ++i)
                io.out[0][i] = io.out[1][i] = next++;
            REQUIRE_FALSE(io.out[2] != nullptr); // Out34 absent on the fake
        }
    } ramp;
    fakes::FakeAudioDevice device;
    std::string info;
    CHECK_FALSE(device.runPeriods(1));
    platform::AudioConfig config;
    config.periodFrames = 32;
    REQUIRE(device.start(config, ramp, info));
    REQUIRE(device.runPeriods(3));
    REQUIRE(device.mainLeft().size() == 96);
    CHECK(device.mainLeft()[95] == 95.0f);
    CHECK(device.stats().periods == 3);
    device.stop();
    CHECK_FALSE(device.started());
}

TEST_CASE("FakeControlSource queues scripted events and triggers the engine on pad presses", "[contracts][fakes]")
{
    struct Sink final : engine::TriggerSink
    {
        std::vector<engine::TriggerNow> hits;
        bool push(const engine::TriggerNow& t) noexcept override
        {
            hits.push_back(t); // test sink: allocation is fine here (not an audio thread)
            return true;
        }
    } sink;
    fakes::FakeControlSource source(&sink);
    std::string info;
    REQUIRE(source.start(info));
    source.emit({io::ControlEvent::Kind::Press, io::pad(5), 110, io::kSourceVirtual, 1.0});
    source.emit({io::ControlEvent::Kind::Turn, io::knob(1), -3, io::kSourceVirtual, 2.0});
    std::vector<io::ControlEvent> events;
    source.poll(events);
    REQUIRE(events.size() == 2);
    CHECK(events[1].value == -3);
    REQUIRE(sink.hits.size() == 1);
    CHECK((sink.hits[0].track == 4 && sink.hits[0].velocity == 110 && sink.hits[0].pad == 4));
    events.clear();
    source.poll(events);
    CHECK(events.empty());
}

TEST_CASE("OffscreenRenderer renders the root HOME screen deterministically", "[contracts][fakes][ui]")
{
    fakes::OffscreenRenderer offscreen;
    std::string error;
    REQUIRE(offscreen.open(1280, 768, error));
    fakes::FakeEngineBridge bridge;
    bridge.meterFeed.masterPeak = {0.25f, 0.25f};
    auto root = ui::createRoot({core::builtinParams(), offscreen.measure(), &bridge, {}});
    REQUIRE(root->id() == "home");
    ui::DrawList dl;
    root->frame(dl, 0.0);
    const auto a = offscreen.draw(dl);
    REQUIRE(a.size() == 1280u * 768u);
    CHECK(a[2 * 1280 + 2] == 0x0B0D10u);   // bg (status row), as test_home_snapshot
    CHECK(a[520 * 1280 + 44] == 0xFF7E72u); // track 1 chip
    const auto b = offscreen.draw(dl);
    CHECK(a == b);

    // knob 1 on the root posts track.volume for the selected track (track 1) to the bridge
    root->handle({io::ControlEvent::Kind::Turn, io::knob(1), -20, io::kSourceVirtual, 0.0}, 0.0);
    REQUIRE(bridge.posted.size() == 1);
    const auto& change = std::get<engine::ParamChange>(bridge.posted[0]);
    CHECK(change.param == *core::builtinParams().idOf("track.volume"));
    CHECK(change.value == 80.0f);
    CHECK(change.scope == 0);
}

TEST_CASE("FakeEffect is a gain and keeps the effect contract", "[contracts][fakes][fx]")
{
    fakes::FakeEffect fx;
    engine::fx::EffectBase& e = fx;
    e.prepare(44100.0, 64);
    e.reset();
    e.setParam(0, 0.25f); // ×0.5
    float l[64], r[64];
    for (int i = 0; i < 64; ++i)
        l[i] = r[i] = 1.0f;
    engine::fx::ProcessContext ctx {44100.0, 120.0, false, 0, {}, 0};
    {
        core::rt::ScopedRealtimeThread realtime;
        e.process(l, r, ctx);
    }
    CHECK(l[63] == 0.5f);
    CHECK(r[0] == 0.5f);
    CHECK(e.latencySamples() == 0);
    CHECK(e.telemetry(0) == 0.5f);
    e.setOption(engine::fx::kClearState, 1);
    CHECK(fx.resets == 1);
}

TEST_CASE("FakeStretcher resamples: length follows the time ratio", "[contracts][fakes][stretch]")
{
    for (double ratio : {0.5, 1.0, 1.5, 2.0})
    {
        fakes::FakeStretcher s;
        s.prepare(44100.0, 64, 1);
        s.setRatios(ratio, 1.0);
        std::vector<float> in(44100), out(static_cast<std::size_t>(44100 * ratio) + 64);
        for (std::size_t i = 0; i < in.size(); ++i)
            in[i] = std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / 44100.0f);
        const float* inPtr[1] = {in.data()};
        float* outPtr[1] = {out.data()};
        const int written = s.process(inPtr, static_cast<int>(in.size()), outPtr, static_cast<int>(out.size()));
        INFO("ratio " << ratio);
        CHECK(written == Catch::Approx(44100.0 * ratio).epsilon(0.01));
        int crossings = 0;
        for (int i = 1; i < written; ++i)
            crossings += (out[static_cast<std::size_t>(i - 1)] < 0) != (out[static_cast<std::size_t>(i)] < 0);
        // resample-only: pitch scales with 1 / ratio (a real MASTER TEMPO backend keeps 440 Hz)
        CHECK(crossings / 2.0 / (written / 44100.0) == Catch::Approx(440.0 / ratio).epsilon(0.02));
        CHECK(s.latencySamples() == 0);
    }
}

TEST_CASE("FakePlatform hands out its fakes", "[contracts][fakes][platform]")
{
    fakes::FakePlatform p;
    p.configure({{"audio_device", "x"}});
    CHECK(p.configured.at("audio_device") == "x");
    CHECK(p.audioDevices().front()->id() == "fake-offline");
    CHECK(p.controlSources({nullptr}).size() == 1);
    CHECK(p.lamps() != nullptr);
    std::string error;
    CHECK(p.display(error)->rendererOrder().front() == "soft");
}
