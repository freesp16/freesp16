// Song mode inside engine::Engine (ARR-004…007): the plan installed by ObjectSwap (SwapSlot::SceneConfig 0/15) and
// arrangement.play, an offline render of the 5-row acceptance song switches on the exact samples, manual scene.queue
// requests are refused while song mode is on, the pattern chain is suspended, the song status is published and retired
// plans are freed on the message thread.
#include "core/util/arrangement/ArrangementPlan.h"
#include "engine/scene/PatternChain.h"
#include "engine/scene/SectionSequencer.h"
#include "tests/unit/arrangement/ArrTestUtil.h"
#include "tests/unit/engine/EngineTestUtil.h"

#include <catch2/catch_test_macros.hpp>

#include <utility>
#include <vector>

namespace es = engine::scene;

namespace
{
core::ParamId id(const char* name)
{
    return core::builtinParams().idOf(name).value_or(core::ParamId {});
}

const std::vector<arrtest::Row> kSong {
    {0, 0, 16, 2, 0.0f}, {0, 1, 8, 3, 140.0f}, {1, 0, 64, 1, 0.0f}, {0, 2, 16, 1, 90.0f}, {1, 1, 8, 2, 0.0f},
};

bool postPlan(engine::Engine& e, const std::vector<arrtest::Row>& rows, core::arrangement::SongStatus* status)
{
    auto* plan = new es::ArrangementPlan();
    std::int64_t at = 0;
    plan->count = static_cast<std::uint16_t>(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        const auto& r = rows[i];
        plan->rows[i] = {static_cast<std::uint8_t>(r.scene), static_cast<std::uint8_t>(r.pattern),
                         static_cast<std::uint8_t>(r.repeats), r.bpm};
        plan->startPpq16[i] = at;
        at += static_cast<std::int64_t>(r.repeats) * r.length;
    }
    plan->startPpq16[rows.size()] = at;
    plan->status = status;
    if (e.bridge().post(engine::ObjectSwap {es::kArrangementSlot, plan}))
        return true;
    es::destroyArrangementPlan(plan);
    return false;
}

void setLengths(engine::Engine& e, const std::vector<arrtest::Row>& rows)
{
    for (const auto& r : rows)
        REQUIRE(e.bridge().post(engine::ParamChange {id("pattern.length"),
                                                     static_cast<std::uint16_t>(r.scene << 4 | r.pattern),
                                                     static_cast<float>(r.length), 0}));
}

// Renders block by block; SwitchDone events as (absolute frame, scene, pattern).
std::vector<arrtest::Switch> render(engine::Engine& e, std::int64_t frames, std::int64_t& at,
                                    std::vector<std::int64_t>* stoppedAt = nullptr)
{
    std::vector<arrtest::Switch> out;
    bool played = false;
    for (std::int64_t end = at + frames; at < end; at += 64)
    {
        enginetest::render(e, 64, 64);
        engine::EngineEvent ev;
        while (e.bridge().popEvent(ev))
            if (ev.kind == engine::EngineEvent::Kind::SwitchDone)
                out.push_back({at + ev.step, (ev.value >> 4) & 15, ev.value & 15});
        const bool playing = e.bridge().playhead().playing;
        if (stoppedAt != nullptr && played && ! playing && stoppedAt->empty())
            stoppedAt->push_back(at);
        played = played || playing;
    }
    return out;
}
} // namespace

TEST_CASE("arr.engine: an offline render of a 5-row song switches at exactly the expected samples, then stops",
          "[arrangement][engine]")
{
    auto e = enginetest::make(false);
    core::arrangement::SongStatus status;
    setLengths(*e, kSong);
    REQUIRE(postPlan(*e, kSong, &status));
    REQUIRE(e->bridge().post(engine::ParamChange {id("arrangement.play"), 0, 1.0f, 0}));
    std::int64_t at = 0;
    render(*e, 64 * 8, at); // stopped: installs the plan, cues row 1
    const std::int64_t origin = at;
    REQUIRE(e->bridge().post(engine::TransportCmd {engine::TransportCmd::Play, 0.0}));
    std::vector<std::int64_t> stopped;
    const auto want = arrtest::expectRows(kSong, 120.0);
    const auto sw = render(*e, want.endFrame + 64 * 40, at, &stopped);
    std::vector<arrtest::Switch> expected;
    for (const auto& [frame, row] : want.rows)
        expected.push_back({origin + frame, kSong[static_cast<std::size_t>(row)].scene,
                            kSong[static_cast<std::size_t>(row)].pattern});
    const std::int64_t stopBlock = origin + (want.endFrame / 64 + 1) * 64;
    expected.push_back({stopBlock, 0, 0}); // stopped at the top: row 1 cued again (immediate)
    CHECK(sw == expected);
    REQUIRE(stopped.size() == 1);
    CHECK(stopped.front() == stopBlock - 64); // the playhead after the end block already shows the stop
    CHECK(status.row.load() == -1);
    CHECK(e->bridge().playhead().bpm == 90.0);
    CHECK(e->load().overruns <= 1 + e->load().blocks / 100); // wall clock on a loaded host (as edit.chain)
    e->bridge().retirePending();
}

TEST_CASE("arr.engine: manual switches are refused and the chain waits while song mode is on", "[arrangement][engine]")
{
    auto e = enginetest::make(false);
    core::arrangement::SongStatus status;
    const std::vector<arrtest::Row> rows {{0, 0, 16, 4, 0.0f}, {0, 3, 16, 1, 0.0f}};
    setLengths(*e, rows);
    REQUIRE(postPlan(*e, rows, &status));
    // A chain 1 → 2 on scene 1: suspended while song mode is on.
    auto* chain = new es::ChainPlan();
    chain->scene = 0;
    chain->length = 2;
    chain->patterns[0] = 0;
    chain->patterns[1] = 1;
    REQUIRE(e->bridge().post(engine::ObjectSwap {es::kChainSlot, chain}));
    REQUIRE(e->bridge().post(engine::ParamChange {id("arrangement.play"), 0, 1.0f, 0}));
    std::int64_t at = 0;
    render(*e, 64 * 4, at);
    REQUIRE(e->bridge().post(engine::TransportCmd {engine::TransportCmd::Play, 0.0}));
    render(*e, 64 * 100, at);
    CHECK(status.row.load() == 0);
    CHECK(status.remaining.load() == 4);
    REQUIRE(e->bridge().post(engine::ParamChange {id("scene.queue"), 0xFFFF, 5.0f, 0})); // pattern 6: refused
    const auto sw = render(*e, 88200 * 4 + 64 * 10, at);
    // Only the song's row change at 4 × 16 steps (352 800 frames after PLAY at frame 256): no pattern 6, no chain.
    REQUIRE(sw.size() == 1);
    CHECK(sw.front() == arrtest::Switch {256 + 352800, 0, 3});
    CHECK(status.row.load() == 1);
    CHECK(status.position16.load() >= 64);
    // Song mode off: the chain plays again and a manual request is taken.
    REQUIRE(e->bridge().post(engine::ParamChange {id("arrangement.play"), 0, 0.0f, 0}));
    REQUIRE(e->bridge().post(engine::ParamChange {id("scene.queue"), 0xFFFF, 5.0f, 0}));
    const auto after = render(*e, 88200 * 2, at);
    REQUIRE_FALSE(after.empty());
    CHECK(after.front().pattern == 5);
    e->bridge().retirePending();
}

TEST_CASE("arr.engine: SHIFT + push starts from a row; loop; a replaced plan is retired", "[arrangement][engine]")
{
    auto e = enginetest::make(false);
    const std::vector<arrtest::Row> rows {{0, 0, 8, 1, 0.0f}, {0, 1, 8, 1, 0.0f}, {0, 2, 8, 1, 0.0f}};
    setLengths(*e, rows);
    REQUIRE(postPlan(*e, rows, nullptr));
    REQUIRE(e->bridge().post(engine::ParamChange {id("arrangement.play"), 0, 1.0f, 0}));
    REQUIRE(e->bridge().post(engine::ParamChange {id("arrangement.loop"), 0, 1.0f, 0}));
    REQUIRE(e->bridge().post(engine::ParamChange {id("arrangement.start"), 0, 2.0f, 0})); // row 3
    std::int64_t at = 0;
    const auto cue = render(*e, 64 * 4, at);
    REQUIRE(cue.size() == 1);
    CHECK(cue.front().pattern == 2);
    REQUIRE(e->bridge().post(engine::TransportCmd {engine::TransportCmd::Play, 0.0}));
    const std::int64_t origin = at;
    const auto sw = render(*e, 44100 * 4, at);
    // Row 3 (2 quarters = 44 100 frames), then the loop: row 1 at 44 100, row 2 at 88 200, row 3 at 132 300, row 1 …
    REQUIRE(sw.size() >= 4);
    CHECK(sw[0] == arrtest::Switch {origin + 44100, 0, 0});
    CHECK(sw[1] == arrtest::Switch {origin + 88200, 0, 1});
    CHECK(sw[2] == arrtest::Switch {origin + 132300, 0, 2});
    CHECK(sw[3] == arrtest::Switch {origin + 176400, 0, 0});
    REQUIRE(postPlan(*e, rows, nullptr)); // the old plan goes to the retire queue
    render(*e, 64 * 4, at);
    e->bridge().retirePending();
    CHECK(e->bridge().playhead().playing);
}
