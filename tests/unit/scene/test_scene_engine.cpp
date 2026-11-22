// The switcher inside engine::Engine (step 5): scene.queue and the scene parameters through the bridge, the PlayheadFeed
// (current, pending, step), SwitchDone events, the pattern mute mask reaching the mixer on the exact boundary frame,
// scene BPM through the transport, and scene isolation (scene 3 values never reach scene 4).
#include "tests/unit/engine/EngineTestUtil.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace
{
core::ParamId id(const char* name)
{
    return core::builtinParams().idOf(name).value_or(core::ParamId {});
}

bool post(engine::Engine& e, const char* param, std::uint16_t scope, float value)
{
    return e.bridge().post(engine::ParamChange {id(param), scope, value, 0});
}

bool play(engine::Engine& e)
{
    return e.bridge().post(engine::TransportCmd {engine::TransportCmd::Play, 0.0});
}

int switchEvents(engine::Engine& e, int& lastValue)
{
    int n = 0;
    engine::EngineEvent ev;
    while (e.bridge().popEvent(ev))
        if (ev.kind == engine::EngineEvent::Kind::SwitchDone)
        {
            ++n;
            lastValue = ev.value;
        }
    return n;
}
} // namespace

TEST_CASE("scene.engine: the scene ids are bound; a queued switch shows as pending, then switches at the pattern end",
          "[scene][engine]")
{
    auto e = enginetest::make(false);
    for (const char* p : {"scene.queue", "project.patternQuantize", "scene.switchMode", "scene.bpm", "scene.bpmPolicy",
                          "pattern.length", "pattern.mutes", "pattern.solos"})
    {
        INFO(p);
        CHECK(id(p).valid());
    }
    REQUIRE(play(*e));
    enginetest::render(*e, 64 * 20, 256); // ≈ 0.06 quarter: inside step 1
    REQUIRE(post(*e, "scene.queue", 0xFFFF, 1.0f)); // scene 1 pattern 2
    enginetest::render(*e, 256, 256);
    const auto& ph = e->bridge().playhead();
    CHECK(ph.pattern == 0);
    CHECK(ph.pendingScene == 0);
    CHECK(ph.pendingPattern == 1);
    CHECK(ph.step == 0);
    int value = -1;
    CHECK(switchEvents(*e, value) == 0);
    enginetest::render(*e, 88200 - 64 * 20 - 256 + 512, 256); // past step 17 (frame 88 200 at 120 BPM)
    CHECK(e->bridge().playhead().pattern == 1);
    CHECK(e->bridge().playhead().pendingPattern == -1);
    CHECK(switchEvents(*e, value) == 1);
    CHECK(value == 1);
}

TEST_CASE("scene.engine: the new pattern's mute mask silences track 1 from the boundary frame exactly",
          "[scene][engine]")
{
    // Reference: the test tone (track 1) with no switch. Switched: pattern 2 mutes track 1 (pattern.mutes bit 0).
    constexpr int kBoundary = 88200, kFrames = kBoundary + 1024;
    auto ref = enginetest::make(true);
    auto sw = enginetest::make(true);
    REQUIRE(post(*sw, "pattern.mutes", 0 << 4 | 1, 1.0f));
    REQUIRE(play(*ref));
    REQUIRE(play(*sw));
    // Queued after the first period (a request in the very block PLAY starts is immediate: nothing has played yet).
    auto a = enginetest::render(*ref, 256, 256);
    auto b = enginetest::render(*sw, 256, 256);
    REQUIRE(post(*sw, "scene.queue", 0xFFFF, 1.0f));
    const auto a2 = enginetest::render(*ref, kFrames - 256, 256);
    const auto b2 = enginetest::render(*sw, kFrames - 256, 256);
    a.l.insert(a.l.end(), a2.l.begin(), a2.l.end());
    b.l.insert(b.l.end(), b2.l.begin(), b2.l.end());
    std::size_t firstDiff = a.l.size();
    for (std::size_t i = 0; i < a.l.size(); ++i)
        if (a.l[i] != b.l[i])
        {
            firstDiff = i;
            break;
        }
    // The mute fade (128 samples, MIX-002) starts on the boundary frame: identical before, silent after the fade.
    CHECK(firstDiff >= static_cast<std::size_t>(kBoundary));
    CHECK(firstDiff <= static_cast<std::size_t>(kBoundary + 1));
    CHECK(enginetest::peak(b.l, kBoundary + 200) == 0.0f);
    CHECK(enginetest::peak(a.l, kBoundary + 200) > 0.2f);
}

TEST_CASE("scene.engine: a scene BPM is applied at the switch; scene 3's values never reach scene 4", "[scene][engine]")
{
    auto e = enginetest::make(false);
    REQUIRE(post(*e, "scene.bpm", 2, 95.0f));             // scene 3 (index 2) has a BPM …
    REQUIRE(post(*e, "pattern.length", 2 << 4 | 0, 32.0f)); // … and a 2-bar pattern 1
    REQUIRE(post(*e, "project.patternQuantize", 0, 0.0f));  // STEP
    REQUIRE(post(*e, "scene.queue", 0xFFFF, 3 * 16 + 0));   // stopped: scene 4 at once
    enginetest::render(*e, 512, 256);
    CHECK(e->bridge().playhead().scene == 3);
    CHECK(e->bridge().playhead().bpm == Catch::Approx(120.0)); // scene 4 has no BPM
    REQUIRE(play(*e));
    enginetest::render(*e, 64 * 1400, 256); // ≈ 4.06 quarters: wrapped once (16 steps)
    CHECK(e->bridge().playhead().step == 0);
    REQUIRE(post(*e, "scene.queue", 0xFFFF, 2 * 16 + 0));
    enginetest::render(*e, 64 * 200, 256); // the next step boundary, then the tempo from the next block
    CHECK(e->bridge().playhead().scene == 2);
    CHECK(e->bridge().playhead().bpm == Catch::Approx(95.0));
    enginetest::render(*e, 64 * 2000, 256);
    CHECK(e->bridge().playhead().step > 16); // 32 steps in scene 3's pattern 1
}

TEST_CASE("scene.engine: 5 min of blocks switching every bar (STEP unit) without allocating", "[scene][engine][soak]")
{
    auto e = enginetest::make(false);
    REQUIRE(post(*e, "project.patternQuantize", 0, 0.0f));
    REQUIRE(play(*e));
    // 120 BPM: one bar = 2 s = 88 200 frames. Render bar by bar (OfflineDriver marks the thread real-time: any
    // allocation aborts under APP_RT_CHECKS); the queue alternates patterns 1 and 2 of scenes 1–16.
    std::vector<float> l(88200), r(88200);
    float* const out[2] = {l.data(), r.data()};
    engine::OfflineDriver d;
    d.periodFrames = 512;
    int switches = 0, value = -1;
    for (int bar = 0; bar < 150; ++bar) // 300 s (the 1 h soak runs the switcher alone: test_scene_soak.cpp)
    {
        const int target = (bar % 16) * 16 + 1 + (bar & 1); // never the current pattern (that would cancel)
        REQUIRE(post(*e, "scene.queue", 0xFFFF, static_cast<float>(target)));
        d.run(*e, 88200, out);
        switches += switchEvents(*e, value);
        if (value != target)
        {
            INFO("bar " << bar);
            REQUIRE(value == target);
        }
    }
    CHECK(switches == 150);
    CHECK(e->blocksRendered() >= 150ull * 88200ull / 64ull);
}
