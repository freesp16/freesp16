// Sampling inside engine::Engine ("REC LENGTH 1–4 bars yields exactly bars × 4 × 60/BPM × fs frames", "first click at
// frame 0 ± 1 after latency compensation", "loop-after-rec starts on the boundary", zero RT violations). Every run goes
// through Engine::process (the calling thread is real-time marked for the call: an allocation aborts under
// APP_RT_CHECKS), with the hub posted through the bridge as the session does.
#include "SamplingTestUtil.h"

#include "services/sampling/TakeBuilder.h"

#include <catch2/catch_test_macros.hpp>

using core::sampling::LiveState;
using core::sampling::RecState;
using namespace smptest;

namespace
{
RecState state(const HubRef& h) { return static_cast<RecState>(h->status.state.load()); }
LiveState liveState(const HubRef& h) { return static_cast<LiveState>(h->status.liveState.load()); }
// Ring index 0 = device frame 64 × k0 (the block that installed the hub).
std::int64_t ringOrigin(engine::Engine& e, const HubRef& h)
{
    return 64 * static_cast<std::int64_t>(e.blocksRendered() - h->status.blocks.load());
}
InputFn noise(float amp = 0.05f)
{
    return [amp](std::int64_t pos, int n, float* l, float* r) {
        for (int i = 0; i < n; ++i)
        {
            std::uint32_t s = static_cast<std::uint32_t>(pos + i) * 2654435761u;
            l[i] = amp * (static_cast<float>(s >> 8) / 8388608.0f - 1.0f);
            r[i] = -l[i];
        }
    };
}
} // namespace

TEST_CASE("sampling.engine: REC LENGTH 1–4 bars is exactly bars × 4 × 60 / BPM × fs frames", "[sampling][engine]")
{
    for (const double bpm : {120.0, 90.0, 100.0})
        for (int bars = 1; bars <= 4; ++bars)
        {
            INFO("bpm " << bpm << " bars " << bars);
            auto e = make();
            auto hub = installHub(*e);
            Driver d;
            d.run(*e, 1024, noise());
            REQUIRE(param(*e, "transport.tempo", static_cast<float>(bpm)));
            REQUIRE(param(*e, "sampling.length", static_cast<float>(bars)));
            REQUIRE(param(*e, "sampling.rec", 1.0f, (0 << 4) | 3));
            d.run(*e, 256, noise());
            REQUIRE(state(hub) == RecState::Recording); // threshold OFF: at once
            const auto expected = static_cast<std::int64_t>(bars * 4 * 60.0 / bpm * 44100.0 + 0.5);
            CHECK(hub->status.takeEnd.load() - hub->status.takeStart.load() == expected);
            CHECK(hub->status.takeBars.load() == bars);
            CHECK(hub->status.takeTrack.load() == 3);
            CHECK(hub->status.takeBpm.load() == static_cast<float>(bpm));
            d.run(*e, expected + 512, noise());
            CHECK(state(hub) == RecState::Done);
            CHECK(hub->ring.head() >= hub->status.takeEnd.load());
        }
}

TEST_CASE("sampling.engine: the hub and REC posted in one go (one drain) start the take at ring frame 0",
          "[sampling][engine]")
{
    auto e = make();
    Driver d;
    d.run(*e, 512, noise());
    auto hub = installHub(*e); // posted, not yet installed
    REQUIRE(param(*e, "sampling.length", 1.0f));
    REQUIRE(param(*e, "sampling.rec", 1.0f, 9));
    d.run(*e, 256, noise());
    CHECK(state(hub) == RecState::Recording);
    CHECK(hub->status.takeStart.load() == 0);
    CHECK(hub->status.takeTrack.load() == 9);
}

TEST_CASE("sampling.engine: REC with OFF length runs until REC again; standby REC cancels", "[sampling][engine]")
{
    auto e = make();
    auto hub = installHub(*e);
    Driver d;
    d.run(*e, 512, noise());
    d.run(*e, 256, {}); // the adapter's last noise block is played out first (64-frame input latency)
    REQUIRE(param(*e, "sampling.threshold", 21.0f)); // −40 dB: silence keeps it in standby
    REQUIRE(param(*e, "sampling.rec", 1.0f));
    d.run(*e, 1024, {}); // no input at all
    CHECK(state(hub) == RecState::Standby);
    const auto seq = hub->status.takeSeq.load();
    REQUIRE(param(*e, "sampling.rec", 0.0f));
    d.run(*e, 256, {});
    CHECK(state(hub) == RecState::Cancelled);

    REQUIRE(param(*e, "sampling.threshold", 0.0f));
    REQUIRE(param(*e, "sampling.rec", 1.0f));
    d.run(*e, 10000, noise());
    CHECK(hub->status.takeSeq.load() == seq + 1);
    CHECK(state(hub) == RecState::Recording);
    CHECK(hub->status.takeEnd.load() == -1);
    const auto start = hub->status.takeStart.load();
    REQUIRE(param(*e, "sampling.rec", 0.0f));
    d.run(*e, 256, noise());
    CHECK(state(hub) == RecState::Done);
    CHECK(hub->status.takeEnd.load() - start >= 10000 - 256);
    CHECK(hub->status.takeEnd.load() - start <= 10000 + 256);
}

TEST_CASE("sampling.engine: REC THRESHOLD −40 dB starts at the first louder frame (pre-roll keeps the lead-in)",
          "[sampling][engine]")
{
    for (const int preRollMs : {0, 10})
    {
        INFO("pre-roll " << preRollMs << " ms");
        auto e = make();
        auto hub = installHub(*e);
        Driver d;
        d.run(*e, 2048, {});
        REQUIRE(param(*e, "sampling.threshold", 21.0f)); // −40 dB = 0.01
        REQUIRE(param(*e, "sampling.preRoll", static_cast<float>(preRollMs)));
        REQUIRE(param(*e, "sampling.rec", 1.0f));
        const std::int64_t click = 12345;
        auto in = [&](std::int64_t pos, int n, float* l, float* r) {
            for (int i = 0; i < n; ++i)
            {
                l[i] = pos + i == click ? 0.02f : 0.001f; // −60 dB floor, one −34 dB frame
                r[i] = 0.0f;
            }
        };
        d.run(*e, 20000, in);
        REQUIRE(state(hub) == RecState::Recording);
        const std::int64_t clickRing = click + 64 - ringOrigin(*e, hub); // + the engine's input block
        CHECK(hub->status.takeStart.load() == clickRing - preRollMs * 441 / 10);
        float x = 0.0f;
        REQUIRE(hub->ring.read(clickRing, 1, &x, nullptr));
        CHECK(x == 0.02f);
    }
}

TEST_CASE("sampling.engine: REC THRESHOLD PLAY starts at the sequencer start, latency compensated", "[sampling][engine]")
{
    for (const int deviceLatency : {0, 300})
    {
        auto e = make();
        auto hub = installHub(*e);
        Driver d;
        d.run(*e, 1024, noise());
        REQUIRE(param(*e, "live.inputLatency", static_cast<float>(deviceLatency)));
        REQUIRE(param(*e, "sampling.threshold", 42.0f));
        REQUIRE(param(*e, "sampling.length", 1.0f));
        REQUIRE(param(*e, "sampling.rec", 1.0f));
        d.run(*e, 3072, noise());
        REQUIRE(state(hub) == RecState::Standby);
        const std::int64_t playAt = d.pos; // multiple of 256: the next block starts the transport
        REQUIRE(e->bridge().post(engine::TransportCmd {engine::TransportCmd::Play, 0.0}));
        d.run(*e, 512, noise());
        REQUIRE(state(hub) == RecState::Recording);
        CHECK(hub->status.takeStart.load() == playAt - ringOrigin(*e, hub) + 64 + deviceLatency);
        CHECK(hub->status.latency.load() == 64 + deviceLatency);
        CHECK(hub->status.takeEnd.load() - hub->status.takeStart.load() == 88200);
    }
}

TEST_CASE("sampling.engine: SOURCE RESAMPLE captures the master exactly; LINE IN L is mono", "[sampling][engine]")
{
    auto e = enginetest::make(true); // the 440 Hz test tone on track 1 → the master
    auto hub = installHub(*e);
    REQUIRE(param(*e, "sampling.source", 3.0f));
    Driver d;
    d.collect = true;
    d.run(*e, 8192, {});
    const std::int64_t origin = ringOrigin(*e, hub);
    std::vector<float> l(4096), r(4096);
    const std::int64_t from = hub->ring.head() - 4096;
    REQUIRE(hub->ring.read(from, 4096, l.data(), r.data()));
    for (int i = 0; i < 4096; ++i)
        REQUIRE(l[static_cast<std::size_t>(i)] == d.mainL[static_cast<std::size_t>(origin + from + i)]);
    CHECK(hub->status.latency.load() == 0);
    CHECK(hub->status.ringChannels.load() == 2);

    REQUIRE(param(*e, "sampling.source", 1.0f)); // LINE IN L
    d.run(*e, 1024, noise());
    CHECK(hub->status.ringChannels.load() == 1);
    REQUIRE(hub->ring.read(hub->ring.head() - 64, 64, l.data(), r.data()));
    for (int i = 0; i < 64; ++i)
        REQUIRE(l[static_cast<std::size_t>(i)] == r[static_cast<std::size_t>(i)]);
}

TEST_CASE("sampling.engine: the post-limiter input reaches the meters (not the raw input)", "[sampling][engine]")
{
    auto e = make();
    auto full = [](std::int64_t pos, int n, float* l, float* r) {
        for (int i = 0; i < n; ++i)
            l[i] = r[i] = (pos + i) % 2 ? 1.0f : -1.0f;
    };
    Driver d;
    d.run(*e, 2048, full);
    CHECK(e->bridge().meters().inputPeak[0] == 1.0f);
    REQUIRE(param(*e, "mixer.softLimiter", 1.0f));
    d.run(*e, 2048, full);
    CHECK(e->bridge().meters().inputPeak[0] <= std::pow(10.0f, -0.3f / 20.0f));
    CHECK(e->sampling().inputStage().clipped());
}

TEST_CASE("sampling.engine: live 2-bar take of a click track — first click at frame 0 ± 1 after latency compensation",
          "[sampling][engine][live]")
{
    for (const int deviceLatency : {0, 117, 1000})
    {
        INFO("device input latency " << deviceLatency);
        auto e = make();
        auto hub = installHub(*e);
        REQUIRE(param(*e, "live.inputLatency", static_cast<float>(deviceLatency)));
        REQUIRE(param(*e, "project.patternQuantize", 1.0f)); // BAR
        REQUIRE(param(*e, "live.mode", 1.0f));
        REQUIRE(param(*e, "live.length", 3.0f)); // 2 bars
        Driver d;
        d.run(*e, 4096, {});
        const std::int64_t playAt = 8192;
        const std::int64_t beat = 22050; // 120 BPM
        // The external gear plays a click on every beat from the transport start; it arrives `deviceLatency` late.
        const InputFn in = clicks(playAt, beat, deviceLatency);
        d.run(*e, playAt - d.pos, in);
        REQUIRE(e->bridge().post(engine::TransportCmd {engine::TransportCmd::Play, 0.0}));
        d.run(*e, 30000, in); // into bar 1
        REQUIRE(param(*e, "live.rec", 1.0f, (0 << 4) | 5));
        d.run(*e, 88200, in); // the take starts on bar 2
        REQUIRE(liveState(hub) == LiveState::Recording);
        const std::int64_t start = hub->status.liveStart.load();
        // On the bar: bar 2 starts at device frame playAt + 88200; + 64 (engine input block) + device latency.
        CHECK(start == playAt + 88200 - ringOrigin(*e, hub) + 64 + deviceLatency);
        CHECK(hub->status.liveLength.load() == 2 * 88200);
        d.run(*e, 2 * 88200 + deviceLatency + 512, in);
        REQUIRE(liveState(hub) == LiveState::Done);

        services::sampling::TakeWindow w {start, 2 * 88200, hub->status.liveMargin.load(), 2, 44100.0};
        CHECK(w.margin == 22050);
        auto built = services::sampling::buildTake(hub->ring, w);
        REQUIRE(built.ok());
        const auto& b = *built.value().buffer;
        const std::int64_t first = firstAbove(b.ch[0] + w.margin, w.length, 0.25f);
        CHECK(std::llabs(first) <= 1);
        CHECK(first == 0);
        // Every beat of the take is on its grid position.
        for (int k = 1; k < 8; ++k)
            CHECK(std::fabs(b.ch[0][w.margin + k * beat]) == 0.5f);
        CHECK(built.value().preCaptured == w.margin);
    }
}

TEST_CASE("sampling.engine: live arm while stopped waits for the start; disarm returns to idle", "[sampling][engine][live]")
{
    auto e = make();
    auto hub = installHub(*e);
    REQUIRE(param(*e, "live.mode", 1.0f));
    REQUIRE(param(*e, "live.length", 0.0f)); // 1/4 bar
    Driver d;
    d.run(*e, 512, noise());
    REQUIRE(param(*e, "live.rec", 1.0f, 2));
    d.run(*e, 4096, noise());
    CHECK(liveState(hub) == LiveState::Armed);
    REQUIRE(param(*e, "live.rec", 0.0f, 2));
    d.run(*e, 256, noise());
    CHECK(liveState(hub) == LiveState::Idle);
    REQUIRE(param(*e, "live.rec", 1.0f, 2));
    d.run(*e, 256, noise());
    REQUIRE(e->bridge().post(engine::TransportCmd {engine::TransportCmd::Play, 0.0}));
    d.run(*e, 512, noise());
    CHECK(liveState(hub) == LiveState::Recording);
    CHECK(hub->status.liveLength.load() == 22050); // 1/4 bar at 120 BPM
    CHECK(hub->status.latency.load() == 64);
}

TEST_CASE("sampling.engine: REC QUANTIZE starts the take on the next 1 / 4-beat boundary, latency compensated",
          "[sampling][engine][quantize]")
{
    // 120 BPM: one beat = 22050 frames. REC arrives 1.36 beats into the pattern (30000 frames after PLAY).
    for (const auto& [index, beats] : {std::pair {1, 1}, std::pair {3, 4}})
    {
        INFO("REC QUANTIZE " << beats << " beats");
        auto e = make();
        auto hub = installHub(*e);
        Driver d;
        d.run(*e, 1024, noise());
        REQUIRE(param(*e, "sampling.quantize", static_cast<float>(index)));
        REQUIRE(param(*e, "sampling.length", 1.0f));
        const std::int64_t playAt = d.pos;
        REQUIRE(e->bridge().post(engine::TransportCmd {engine::TransportCmd::Play, 0.0}));
        d.run(*e, 30000, noise());
        REQUIRE(param(*e, "sampling.rec", 1.0f, 5));
        d.run(*e, 256, noise());
        REQUIRE(state(hub) == RecState::Standby);
        CHECK(hub->status.takeQuantize.load() == beats);
        // The countdown: from this block to the boundary (2 − 1.37…, or 4 − 1.37…), in beats.
        const float left = hub->status.startInBeats.load();
        CHECK(left > static_cast<float>(beats == 1 ? 0.5 : 2.5));
        CHECK(left < static_cast<float>(beats == 1 ? 0.65 : 2.65));
        const std::int64_t boundary = (beats == 1 ? 2 : 4) * 22050;
        d.run(*e, boundary - 30000 - 256 + 1024, noise());
        REQUIRE(state(hub) == RecState::Recording);
        CHECK(hub->status.takeStart.load() == playAt - ringOrigin(*e, hub) + boundary + 64);
        CHECK(hub->status.takeEnd.load() - hub->status.takeStart.load() == 88200); // REC LENGTH 1 bar from there
    }
}

TEST_CASE("sampling.engine: REC QUANTIZE while stopped waits for PLAY (the pattern start), not the threshold",
          "[sampling][engine][quantize]")
{
    auto e = make();
    auto hub = installHub(*e);
    Driver d;
    d.run(*e, 1024, noise());
    REQUIRE(param(*e, "sampling.threshold", 1.0f)); // −60 dB: the noise would start it at once without the quantize
    REQUIRE(param(*e, "sampling.quantize", 5.0f));  // 16 beats
    REQUIRE(param(*e, "sampling.rec", 1.0f, 2));
    d.run(*e, 4096, noise());
    REQUIRE(state(hub) == RecState::Standby);
    CHECK(hub->status.startInBeats.load() == -1.0f); // stopped: the UI shows WAITING FOR PLAY
    const std::int64_t playAt = d.pos;
    REQUIRE(e->bridge().post(engine::TransportCmd {engine::TransportCmd::Play, 0.0}));
    d.run(*e, 512, noise());
    REQUIRE(state(hub) == RecState::Recording);
    CHECK(hub->status.takeStart.load() == playAt - ringOrigin(*e, hub) + 64);
}

TEST_CASE("sampling.engine: REC QUANTIZE 16 beats on a 1-bar pattern starts at the next pattern start",
          "[sampling][engine][quantize]")
{
    auto e = make();
    auto hub = installHub(*e);
    Driver d;
    d.run(*e, 1024, noise());
    REQUIRE(param(*e, "sampling.quantize", 5.0f));
    const std::int64_t playAt = d.pos;
    REQUIRE(e->bridge().post(engine::TransportCmd {engine::TransportCmd::Play, 0.0}));
    d.run(*e, 30000, noise());
    REQUIRE(param(*e, "sampling.rec", 1.0f));
    d.run(*e, 4 * 22050 - 30000 + 1024, noise()); // the default pattern is 16 steps = 4 beats
    REQUIRE(state(hub) == RecState::Recording);
    CHECK(hub->status.takeStart.load() == playAt - ringOrigin(*e, hub) + 4 * 22050 + 64);
}

TEST_CASE("sampling.engine: drop throws the take away in standby or recording; another track's drop is ignored",
          "[sampling][engine][quantize]")
{
    auto e = make();
    auto hub = installHub(*e);
    Driver d;
    d.run(*e, 1024, noise());
    REQUIRE(param(*e, "sampling.quantize", 3.0f));
    REQUIRE(param(*e, "sampling.rec", 1.0f, (0 << 4) | 6));
    d.run(*e, 256, noise());
    REQUIRE(state(hub) == RecState::Standby); // stopped: waits for PLAY
    REQUIRE(param(*e, "sampling.drop", 1.0f, (0 << 4) | 7)); // pad 8: not this take
    d.run(*e, 256, noise());
    CHECK(state(hub) == RecState::Standby);
    REQUIRE(param(*e, "sampling.drop", 1.0f, (0 << 4) | 6));
    d.run(*e, 256, noise());
    CHECK(state(hub) == RecState::Cancelled);

    REQUIRE(param(*e, "sampling.quantize", 0.0f)); // OFF: at once
    REQUIRE(param(*e, "sampling.rec", 1.0f, (0 << 4) | 6));
    d.run(*e, 4096, noise());
    REQUIRE(state(hub) == RecState::Recording);
    REQUIRE(param(*e, "sampling.drop", 1.0f, (0 << 4) | 6));
    d.run(*e, 256, noise());
    CHECK(state(hub) == RecState::Cancelled);

    // A take that already ended is kept.
    REQUIRE(param(*e, "sampling.length", 1.0f));
    REQUIRE(param(*e, "sampling.rec", 1.0f, (0 << 4) | 6));
    d.run(*e, 88200 + 1024, noise());
    REQUIRE(state(hub) == RecState::Done);
    REQUIRE(param(*e, "sampling.drop", 1.0f, (0 << 4) | 6));
    d.run(*e, 256, noise());
    CHECK(state(hub) == RecState::Done);
}
