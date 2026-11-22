// Audio → UI feeds (threading.md "Audio → UI: triple-buffered snapshots"; audio-engine.md steps 13–14): per-period
// meters (linear peak / RMS of the master, per-track peaks), the playhead, published once per device period, and a
// reader on another thread that never sees a torn snapshot.
#include "tests/unit/engine/EngineTestUtil.h"

#include "engine/feeds/Feeds.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>

TEST_CASE("feeds: master and track meters of the test tone", "[engine][feeds]")
{
    auto e = enginetest::make();
    enginetest::render(*e, 4410, 441); // whole periods of 441 frames (10 ms)
    const auto& m = e->bridge().meters();
    CHECK(m.blockIndex == e->blocksRendered());
    CHECK(m.masterPeak[0] == Catch::Approx(0.25).epsilon(0.01));
    CHECK(m.masterPeak[1] == m.masterPeak[0]);
    // RMS over the blocks rendered in the last period: a sine of peak 0.25 → 0.25 / √2
    CHECK(m.masterRms[0] == Catch::Approx(0.25 / std::sqrt(2.0)).epsilon(0.03));
    CHECK(m.trackPeak[0][0] == Catch::Approx(0.25).epsilon(0.01));
    for (int t = 1; t < engine::kTracks; ++t)
        CHECK(m.trackPeak[static_cast<std::size_t>(t)][0] == 0.0f); // silent tracks
    CHECK(m.sendPeak[0] == 0.0f);
    CHECK(m.inputPeak[0] == 0.0f);
}

TEST_CASE("feeds: silence meters zero; the playhead counts blocks", "[engine][feeds]")
{
    auto e = enginetest::make(false);
    enginetest::render(*e, 1000, 100);
    const auto& m = e->bridge().meters();
    CHECK(m.masterPeak[0] == 0.0f);
    CHECK(m.masterRms[0] == 0.0f);
    const auto& p = e->bridge().playhead();
    CHECK(p.blockIndex == (1000 + 63) / 64);
    CHECK_FALSE(p.playing);
    CHECK(p.trackPosition[0] < 0.0f); // idle
}

TEST_CASE("feeds: MeterAccumulator keeps the max of the period and resets after publishing", "[engine][feeds]")
{
    engine::feeds::MeterAccumulator acc;
    alignas(16) float a[64] {}, b[64] {};
    a[10] = -0.5f;
    b[3] = 0.75f;
    acc.addMaster(a, b, 64);
    a[10] = 0.1f;
    acc.addMaster(a, b, 64);
    acc.addTrack(5, b, a, 64);
    engine::MeterFeed m {};
    acc.fill(m, 42);
    CHECK(m.blockIndex == 42);
    CHECK(m.masterPeak[0] == 0.5f);
    CHECK(m.masterPeak[1] == 0.75f);
    CHECK(m.masterRms[1] == Catch::Approx(std::sqrt(2 * 0.75 * 0.75 / 128.0)));
    CHECK(m.trackPeak[5][0] == 0.75f);
    CHECK(m.trackPeak[5][1] == 0.1f);
    engine::MeterFeed n {};
    acc.fill(n, 43);
    CHECK(n.masterPeak[0] == 0.0f);
    CHECK(n.masterRms[0] == 0.0f);
}

TEST_CASE("feeds: a UI thread reads consistent snapshots while the audio thread publishes", "[engine][feeds]")
{
    engine::feeds::EngineFeeds feeds;
    std::atomic<bool> stop {false};
    std::atomic<int> torn {0}, reads {0};
    std::thread ui([&] {
        std::uint64_t last = 0;
        while (! stop.load())
        {
            const auto& p = feeds.playhead();
            // the writer stores blockIndex in both places: a torn read would differ
            if (static_cast<std::uint64_t>(p.ppq) != p.blockIndex || p.blockIndex < last)
                torn.fetch_add(1);
            last = p.blockIndex;
            reads.fetch_add(1);
        }
    });
    while (reads.load() == 0) // Release builds publish 200k blocks in ~1 ms: make sure the reader really overlaps
        std::this_thread::yield();
    engine::PlayheadFeed p {};
    for (std::uint64_t i = 1; i <= 200000; ++i)
    {
        p.ppq = static_cast<double>(i);
        feeds.publish(i, p);
    }
    stop = true;
    ui.join();
    CHECK(torn == 0);
    CHECK(reads > 0);
    CHECK(feeds.playhead().blockIndex == 200000);
}

TEST_CASE("feeds: engine events are lossless up to the queue size", "[engine][feeds]")
{
    engine::feeds::EngineFeeds feeds;
    for (int i = 0; i < static_cast<int>(engine::feeds::EngineFeeds::kEventQueueSize); ++i)
        REQUIRE(feeds.pushEvent({engine::EngineEvent::Kind::RecordedHit, 1, static_cast<std::uint8_t>(i & 63),
                                 static_cast<std::int16_t>(i), 0}));
    CHECK_FALSE(feeds.pushEvent({engine::EngineEvent::Kind::Overload}));
    CHECK(feeds.droppedEvents() == 1);
    engine::EngineEvent e {};
    int n = 0;
    while (feeds.popEvent(e))
        CHECK(e.value == n++);
    CHECK(n == static_cast<int>(engine::feeds::EngineFeeds::kEventQueueSize));
}
