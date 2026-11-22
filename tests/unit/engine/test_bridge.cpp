// The engine half of EngineBridge (threading.md "Channels between threads", "Object swap protocol"):
// post() validation, ObjectSwap install at block start + retire, never freeing on the audio thread (RT hook), the grace
// period, shutdown, transport, and the multi-producer TriggerSink.
#include "tests/unit/engine/EngineTestUtil.h"

#include "core/util/Realtime.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>

namespace
{
struct Payload
{
    int id = 0;
};
std::atomic<int> destroyed {0};
std::atomic<int> destroyedOnRealtimeThread {0};
void destroyPayload(const void* p) noexcept
{
    if (core::rt::isRealtimeThread())
        destroyedOnRealtimeThread.fetch_add(1);
    destroyed.fetch_add(1);
    delete static_cast<const Payload*>(p); // with APP_RT_CHECKS a delete on the audio thread would abort the test
}
// Generic payload slots: kinds with no engine consumer of their own (TrackSample / Preview hold SampleBuffers for the
// voices, voice::acceptVoiceSwaps).
const engine::SwapSlot preview {engine::SwapSlot::SceneConfig, 0, 0};
} // namespace

TEST_CASE("bridge: post() refuses what the engine cannot use", "[engine][bridge]")
{
    auto e = enginetest::make();
    auto& b = e->bridge();
    CHECK(b.post(engine::ParamChange {enginetest::volumeId(), 0, 80.0f, 64}));
    CHECK_FALSE(b.post(engine::ParamChange {core::ParamId {}, 0, 1.0f, 0}));        // invalid id
    CHECK_FALSE(b.post(engine::ParamChange {core::ParamId {1999}, 0, 1.0f, 0}));    // not bound
    int object = 0;
    // no consumer: every kind has one now (LiveTake = the sampling capture hub, SliceMap = the slice tracks), so a kind
    // value far outside the enum (robust to appended kinds)
    CHECK_FALSE(b.post(engine::ObjectSwap {{static_cast<engine::SwapSlot::Kind>(0xFF), 0, 0}, &object}));
    e->acceptSwaps(engine::SwapSlot::LiveTake, &destroyPayload);
    CHECK_FALSE(b.post(engine::ObjectSwap {{engine::SwapSlot::LiveTake, 16, 0}, &object})); // scene out of range
    CHECK_FALSE(b.post(engine::ObjectSwap {{engine::SwapSlot::LiveTake, 0, 16}, &object})); // track out of range
    CHECK_FALSE(b.post(engine::TriggerNow {16, 100, 60, -1}));
    CHECK(b.post(engine::TriggerNow {15, 100, 60, -1}));
    CHECK(b.post(engine::TransportCmd {engine::TransportCmd::SetTempo, 133.0}));
}

TEST_CASE("bridge: the FIFO holds 4096 messages and refuses the next one", "[engine][bridge]")
{
    auto e = enginetest::make();
    auto& b = e->bridge();
    int accepted = 0;
    while (b.post(engine::ParamChange {enginetest::volumeId(), 1, 100.0f, 64}) && accepted < 10000)
        ++accepted;
    CHECK(accepted == static_cast<int>(engine::kMessageQueueSize));
    CHECK_FALSE(b.post(engine::ParamChange {enginetest::volumeId(), 1, 100.0f, 64})); // nothing drained yet
    enginetest::render(*e, 64, 64); // one block drains 512 (EngineOptions::maxMessagesPerBlock)
    int again = 0;
    while (b.post(engine::ParamChange {enginetest::volumeId(), 1, 100.0f, 64}) && again < 10000)
        ++again;
    CHECK(again == 512);
}

TEST_CASE("bridge: ObjectSwap installs at block start, retires the old object, frees it off the audio thread",
          "[engine][bridge][realtime]")
{
    destroyed = 0;
    destroyedOnRealtimeThread = 0;
    auto e = enginetest::make();
    e->acceptSwaps(engine::SwapSlot::SceneConfig, &destroyPayload);
    auto& b = e->bridge();
    auto* first = new Payload {1};
    auto* second = new Payload {2};
    REQUIRE(b.post(engine::ObjectSwap {preview, first}));
    CHECK(e->installed(preview) == nullptr); // not before the next block
    enginetest::render(*e, 64, 64);
    CHECK(e->installed(preview) == first);

    REQUIRE(b.post(engine::ObjectSwap {preview, second}));
    enginetest::render(*e, 64, 64); // installs `second`, retires `first` at this block
    CHECK(e->installed(preview) == second);
    CHECK(destroyed == 0);
    b.retirePending(); // < 2 blocks since the retire: kept
    CHECK(destroyed == 0);
    enginetest::render(*e, 128, 64);
    b.retirePending(); // >= 2 blocks: freed, on this (message) thread
    CHECK(destroyed == 1);
    CHECK(destroyedOnRealtimeThread == 0);

    // a null object clears the slot (the old one retires)
    REQUIRE(b.post(engine::ObjectSwap {preview, nullptr}));
    enginetest::render(*e, 64 * 3, 64);
    b.retirePending();
    CHECK(e->installed(preview) == nullptr);
    CHECK(destroyed == 2);
#if APP_RT_CHECKS
    CHECK(core::rt::violationCount() == 0);
#endif
}

TEST_CASE("bridge: shutdown frees installed, retired and still-queued objects", "[engine][bridge]")
{
    destroyed = 0;
    {
        auto e = enginetest::make();
        e->acceptSwaps(engine::SwapSlot::SceneConfig, &destroyPayload);
        e->acceptSwaps(engine::SwapSlot::LiveTake, &destroyPayload);
        auto& b = e->bridge();
        REQUIRE(b.post(engine::ObjectSwap {preview, new Payload {1}}));
        enginetest::render(*e, 64, 64);
        REQUIRE(b.post(engine::ObjectSwap {preview, new Payload {2}})); // retires #1
        enginetest::render(*e, 64, 64);
        REQUIRE(b.post(engine::ObjectSwap {{engine::SwapSlot::LiveTake, 3, 7}, new Payload {3}})); // never installed
        CHECK(destroyed == 0);
    }
    CHECK(destroyed == 3);
}

TEST_CASE("bridge: a full retire queue keeps the old object installed and the order of later messages",
          "[engine][bridge]")
{
    destroyed = 0;
    auto e = enginetest::make();
    e->acceptSwaps(engine::SwapSlot::SceneConfig, &destroyPayload);
    auto& b = e->bridge();
    // 1025 swaps without a collector: the retire queue (1024) fills; the 1026th swap waits, nothing is lost or freed.
    constexpr int swaps = 1100;
    int posted = 0;
    for (int i = 0; i < swaps; ++i)
        posted += b.post(engine::ObjectSwap {preview, new Payload {i}}) ? 1 : 0;
    REQUIRE(posted == swaps);
    enginetest::render(*e, 64 * 4, 64);
    CHECK(destroyed == 0);
    CHECK(static_cast<const Payload*>(e->installed(preview))->id == 1024); // #0…#1023 retired, #1024 installed
    // the collector catches up; the carried swap and the rest go through in order
    for (int round = 0; round < 4; ++round)
    {
        enginetest::render(*e, 64 * 3, 64);
        b.retirePending();
    }
    CHECK(static_cast<const Payload*>(e->installed(preview))->id == swaps - 1);
    CHECK(destroyed == swaps - 1);
}

TEST_CASE("bridge: transport commands reach the block context and the playhead feed", "[engine][bridge][feeds]")
{
    auto e = enginetest::make(false);
    auto& b = e->bridge();
    REQUIRE(b.post(engine::TransportCmd {engine::TransportCmd::SetTempo, 120.0}));
    REQUIRE(b.post(engine::TransportCmd {engine::TransportCmd::Play, 0.0}));
    enginetest::render(*e, 44100, 256);
    const auto& p = b.playhead();
    CHECK(p.playing);
    CHECK(p.bpm == 120.0);
    // 690 blocks × 64 frames at 120 bpm = 2.0034 quarter notes
    CHECK(std::fabs(p.ppq - 690.0 * 64.0 * 2.0 / 44100.0) < 1e-9);
    REQUIRE(b.post(engine::TransportCmd {engine::TransportCmd::Stop, 0.0}));
    enginetest::render(*e, 256, 256);
    CHECK_FALSE(b.playhead().playing);
}

TEST_CASE("bridge: the TriggerSink takes hits from several threads at once", "[engine][bridge]")
{
    auto e = enginetest::make(true); // the ping exists only with the test tone (the tone alone peaks at 0.25)
    constexpr int threads = 4, perThread = 60; // 240 < 256 = the sink's capacity
    std::vector<std::thread> producers;
    std::atomic<int> refused {0}; // Catch2 3.7 assertions are not thread-safe: count here, CHECK after the join
    for (int t = 0; t < threads; ++t)
        producers.emplace_back([&, t] {
            for (int i = 0; i < perThread; ++i)
                if (!e->triggers().push({static_cast<std::uint8_t>(t), 100, 60, static_cast<std::int8_t>(t)}))
                    refused.fetch_add(1, std::memory_order_relaxed);
        });
    for (auto& p : producers)
        p.join();
    CHECK(refused.load() == 0);
    CHECK_FALSE(e->triggers().push({16, 100, 60, 0})); // track out of range
    const auto out = enginetest::render(*e, 64 * 8, 64); // ≤ 64 per block → all within 4 blocks
    CHECK(e->triggersReceived() == threads * perThread);
    CHECK(enginetest::peak(out.l) > 0.3f); // tone 0.25 + the test ping (velocity 100 → 0.39)
}

TEST_CASE("bridge: events and retirePending without a running engine are harmless", "[engine][bridge]")
{
    auto e = enginetest::make();
    engine::EngineEvent ev {};
    CHECK_FALSE(e->bridge().popEvent(ev));
    e->bridge().retirePending();
}
