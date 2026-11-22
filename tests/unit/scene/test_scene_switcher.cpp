// engine::scene::ScenePatternSwitcher: queued switching at STEP / BAR / PATTERN boundaries (SCN-020), cancel
// (SCN-021), stopped = immediate (SCN-024), RESTART / CONTINUE (SCN-025), scene BPM (SCN-004/005), pattern masks to
// the mixer at the switch frame (SCN-012), switchable (SCN-023), the boundary contract the engine stages rely on.
#include "tests/unit/scene/SceneTestUtil.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using engine::scene::SwitchUnit;
using scenetest::Rig;

namespace
{
// The absolute frame of PPQ `q` from a start at PPQ 0 (the boundary-contract rounding: ⌊q / ppqPerFrame + 1e-6⌋).
std::int64_t frameOf(double q, double bpm, double rate = 44100.0)
{
    return static_cast<std::int64_t>(q / (bpm / (60.0 * rate)) + 1e-6);
}
} // namespace

TEST_CASE("scene.switch.pattern: a PATTERN-unit switch requested at step 5 lands on step 17 (step 1 of the next pass)",
          "[scene]")
{
    for (const double bpm : {120.0, 90.0, 133.0, 300.0})
    {
        INFO("bpm " << bpm);
        Rig r;
        r.clock.bpm = bpm;
        r.clock.play();
        r.runUntilPpq(1.0); // step 5 of a 16-step pattern (steps are 0.25 quarter)
        REQUIRE(r.sw.state().pattern == 0);
        r.sw.request(0, 1);
        CHECK(r.sw.state().pendingPattern == 1);
        REQUIRE(r.runUntilSwitch());
        CHECK(r.lastSwitchFrame == frameOf(4.0, bpm)); // ± 0 frames
        CHECK(r.timing.boundary.has);
        CHECK(r.timing.boundary.ppq == Catch::Approx(4.0).margin(1e-12));
        CHECK(r.timing.pattern == 1);
        CHECK(r.timing.patternStartPpq == Catch::Approx(4.0).margin(1e-12));
        CHECK_FALSE(r.timing.sceneChanged);
        CHECK(r.sw.state().pendingPattern == -1);
        CHECK(r.sw.previous().pattern == 0); // frames before the boundary still belong to pattern 1
    }
    // 120 BPM at 44.1 kHz: step 17 = 2 s = frame 88 200 exactly.
    CHECK(frameOf(4.0, 120.0) == 88200);
}

TEST_CASE("scene.switch.bar: BAR unit switches at the next bar of the pass; a short pattern ends its bar early",
          "[scene]")
{
    Rig r;
    r.sw.setUnit(SwitchUnit::Bar);
    r.sw.setPatternLength(0, 0, 32); // 2 bars
    r.clock.play();
    r.runUntilPpq(0.3); // step 2
    r.sw.request(0, 3);
    REQUIRE(r.runUntilSwitch());
    CHECK(r.lastSwitchFrame == frameOf(4.0, 120.0)); // bar 2, not the pattern end (8.0)
    CHECK(r.timing.pattern == 3);

    Rig s;
    s.sw.setUnit(SwitchUnit::Bar);
    s.sw.setPatternLength(0, 0, 12); // 3 beats: the pass ends before its first bar line
    s.clock.play();
    s.runUntilPpq(0.6);
    s.sw.request(0, 1);
    REQUIRE(s.runUntilSwitch());
    CHECK(s.lastSwitchFrame == frameOf(3.0, 120.0));
}

TEST_CASE("scene.switch.step: STEP unit switches at the next step; RESTART starts the new pattern at step 1",
          "[scene]")
{
    Rig r;
    r.sw.setUnit(SwitchUnit::Step);
    r.clock.play();
    r.runUntilPpq(1.1); // inside step 5 (1.0–1.25)
    r.sw.request(0, 2);
    REQUIRE(r.runUntilSwitch());
    CHECK(r.lastSwitchFrame == frameOf(1.25, 120.0));
    CHECK(r.timing.patternStartPpq == Catch::Approx(1.25).margin(1e-12)); // step 1 of pattern 3 on the boundary
    CHECK(r.sw.stepAt(1.25) == 0);
}

TEST_CASE("scene.switch.continue: CONTINUE keeps the step position modulo the new length", "[scene]")
{
    Rig r;
    r.sw.setUnit(SwitchUnit::Step);
    r.sw.setContinue(true);
    r.sw.setPatternLength(0, 1, 8);
    r.clock.play();
    r.runUntilPpq(16.0 * 0.25 + 10 * 0.25 + 0.1); // second pass, inside step 11
    r.sw.request(0, 1);
    REQUIRE(r.runUntilSwitch());
    // The boundary is step 12 of the old pattern (index 11); modulo 8 → index 3 of the new one.
    CHECK(r.timing.boundary.ppq == Catch::Approx(4.0 + 11 * 0.25).margin(1e-12));
    CHECK(r.sw.stepAt(r.timing.boundary.ppq) == 3);

    // PATTERN unit + CONTINUE: the pass end is position 0, so the new pattern still starts at step 1.
    Rig p;
    p.sw.setContinue(true);
    p.sw.setPatternLength(0, 1, 12);
    p.clock.play();
    p.runUntilPpq(1.3);
    p.sw.request(0, 1);
    REQUIRE(p.runUntilSwitch());
    CHECK(p.sw.stepAt(4.0) == 0);
    CHECK(p.timing.patternStartPpq == Catch::Approx(4.0).margin(1e-12));
}

TEST_CASE("scene.switch.cancel: cancel, or the current pattern again, before the boundary leaves the pattern", "[scene]")
{
    Rig r;
    r.clock.play();
    r.runUntilPpq(1.0);
    r.sw.request(0, 5);
    r.runUntilPpq(2.0);
    r.sw.cancel();
    r.runUntilPpq(9.0); // two pattern ends passed
    CHECK(r.lastSwitchFrame == -1);
    CHECK(r.sw.state().pattern == 0);

    r.sw.request(0, 5);
    r.sw.request(0, 0); // selecting the current pattern again (SCN-021)
    CHECK(r.sw.state().pendingPattern == -1);
    r.runUntilPpq(17.0);
    CHECK(r.lastSwitchFrame == -1);

    r.sw.applyQueue(5.0f); // the scene.queue encoding: request …
    CHECK(r.sw.state().pendingPattern == 5);
    r.sw.applyQueue(-1.0f); // … cancel
    CHECK(r.sw.state().pendingPattern == -1);
}

TEST_CASE("scene.switch.stopped: a switch while stopped is immediate, at the next block start", "[scene]")
{
    Rig r;
    r.step();
    r.sw.request(3, 7);
    const auto res = r.step();
    CHECK(res.switched);
    CHECK(res.frame == 0);
    CHECK_FALSE(r.timing.boundary.has);
    CHECK(r.timing.scene == 3);
    CHECK(r.timing.pattern == 7);
    CHECK(r.timing.sceneChanged);
    REQUIRE(r.mixer.scenes.size() == 1);
    CHECK(r.mixer.scenes[0].scene == 3);
    // PLAY from the top: the new pattern's pass starts at PPQ 0.
    r.clock.play();
    r.step();
    CHECK(r.timing.patternStartPpq == 0.0);
    CHECK_FALSE(r.timing.sceneChanged);

    // Paused at a position, then switched (RESTART): step 1 sits where CONTINUE resumes.
    r.runUntilPpq(5.3);
    r.clock.stop();
    r.step();
    r.sw.request(3, 8);
    r.step();
    CHECK(r.timing.patternStartPpq == Catch::Approx(r.clock.stoppedPpq).margin(1e-12));
    r.clock.play(r.clock.stoppedPpq); // Continue
    r.step();
    CHECK(r.sw.stepAt(r.clock.stoppedPpq) == 0);
}

TEST_CASE("scene.switch.bpm: a scene with BPM 90 sets the tempo at the boundary (APPLY); IGNORE / ASK do not",
          "[scene]")
{
    Rig r;
    r.sw.setSceneBpm(1, 90.0f);
    r.clock.play();
    r.runUntilPpq(1.0);
    r.sw.request(1, 0);
    REQUIRE(r.runUntilSwitch());
    CHECK(r.lastSwitchFrame == frameOf(4.0, 120.0));
    CHECK(r.timing.sceneChanged);
    REQUIRE(r.transport.commands.size() == 1);
    CHECK(r.transport.commands[0].kind == engine::TransportCmd::SetTempo);
    CHECK(r.transport.commands[0].value == Catch::Approx(90.0));
    REQUIRE(r.mixer.scenes.size() == 1);
    CHECK(r.mixer.scenes[0].scene == 1);
    CHECK(r.mixer.scenes[0].frame == r.timing.boundary.frame);

    // Back to scene 1 (no BPM): the running tempo stays.
    r.sw.request(0, 0);
    REQUIRE(r.runUntilSwitch());
    CHECK(r.transport.commands.size() == 1);

    // IGNORE / ASK (follow off): no tempo, unless the request carries "apply" (the ASK popup answered yes).
    r.sw.setFollowSceneBpm(false);
    r.sw.request(1, 0);
    REQUIRE(r.runUntilSwitch());
    CHECK(r.transport.commands.size() == 1);
    r.sw.request(0, 0);
    REQUIRE(r.runUntilSwitch());
    r.sw.applyQueue(static_cast<float>(1 * 16 + 0 + engine::scene::kQueueApplyBpm));
    REQUIRE(r.runUntilSwitch());
    REQUIRE(r.transport.commands.size() == 2);
    CHECK(r.transport.commands[1].value == Catch::Approx(90.0));
    // A pattern switch inside the scene never touches the tempo.
    r.sw.setFollowSceneBpm(true);
    r.sw.request(1, 4);
    REQUIRE(r.runUntilSwitch());
    CHECK(r.transport.commands.size() == 2);
}

TEST_CASE("scene.switch.masks: the new pattern's mute/solo masks reach the mixer at the switch frame", "[scene]")
{
    Rig r;
    r.sw.setPatternMuteSolo(0, 2, 0x0003, 0x0010);
    r.step(); // the first block sends the current pattern's (empty) masks once
    REQUIRE(r.mixer.masks.size() == 1);
    CHECK(r.mixer.masks[0].mute == 0);
    r.clock.play();
    r.runUntilPpq(0.5);
    r.sw.request(0, 2);
    REQUIRE(r.runUntilSwitch());
    REQUIRE(r.mixer.masks.size() == 2);
    CHECK(r.mixer.masks[1].mute == 0x0003);
    CHECK(r.mixer.masks[1].solo == 0x0010);
    CHECK(r.mixer.masks[1].frame == r.timing.boundary.frame);
    CHECK(r.mixer.scenes.empty()); // same scene: the strips keep their parameter block
    // Editing the playing pattern's masks: heard at the next block start; another pattern's edit is not sent.
    r.sw.setPatternMuteSolo(0, 2, 0x0001, 0);
    r.sw.setPatternMuteSolo(0, 9, 0xFFFF, 0);
    r.step();
    REQUIRE(r.mixer.masks.size() == 3);
    CHECK(r.mixer.masks[2].mute == 0x0001);
    CHECK(r.mixer.masks[2].frame == 0);
    r.step();
    CHECK(r.mixer.masks.size() == 3);
}

TEST_CASE("scene.switch.switchable: blocked switching ignores requests and holds a queued one", "[scene]")
{
    Rig r;
    r.clock.play();
    r.runUntilPpq(1.0);
    r.sw.request(0, 4);
    r.sw.applyQueue(static_cast<float>(engine::scene::kQueueBlock));
    CHECK_FALSE(r.sw.switchable());
    r.sw.request(0, 6); // ignored (SCN-023 / SCN-051)
    r.runUntilPpq(9.0);
    CHECK(r.lastSwitchFrame == -1);
    CHECK(r.sw.state().pendingPattern == 4);
    r.sw.applyQueue(static_cast<float>(engine::scene::kQueueUnblock));
    REQUIRE(r.runUntilSwitch());
    CHECK(r.lastSwitchFrame == frameOf(12.0, 120.0));
    CHECK(r.timing.pattern == 4);
}

TEST_CASE("scene.switch.events: every switch reports SwitchDone (scene × 16 + pattern) once", "[scene]")
{
    Rig r;
    r.sw.request(2, 9);
    r.step();
    r.step();
    REQUIRE(r.events.events.size() == 1);
    CHECK(r.events.events[0].kind == engine::EngineEvent::Kind::SwitchDone);
    CHECK(r.events.events[0].value == 2 * 16 + 9);
}

TEST_CASE("scene.switch.wrap: passes wrap without a boundary; a shortened pattern and a locate keep the grid",
          "[scene]")
{
    Rig r;
    r.clock.play();
    r.runUntilPpq(40.1);
    CHECK_FALSE(r.timing.boundary.has);
    CHECK(r.timing.patternStartPpq == Catch::Approx(40.0).margin(1e-12)); // pass 11 of a 16-step pattern
    CHECK(r.sw.stepAt(41.3) == 5);
    r.sw.setPatternLength(0, 0, 8); // 2 beats: passes at 40, 42, …
    r.runUntilPpq(43.0);
    CHECK(r.timing.patternStartPpq == Catch::Approx(42.0).margin(1e-12));
    // A locate backwards (and CONTINUE) steps back whole passes.
    r.clock.play(5.0);
    r.clock.startEdge = false;
    r.step();
    CHECK(r.timing.patternStartPpq == Catch::Approx(4.0).margin(1e-12));
    // The PlayheadFeed fields.
    engine::PlayheadFeed f {};
    r.sw.request(0, 3);
    r.sw.publish(f, 5.3);
    CHECK(f.scene == 0);
    CHECK(f.pattern == 0);
    CHECK(f.step == 5);
    CHECK(f.pendingScene == 0);
    CHECK(f.pendingPattern == 3);
}

TEST_CASE("scene.switch.precount: counting in plays no pattern — a switch is immediate, the pass starts at PPQ 0",
          "[scene]")
{
    Rig r;
    r.clock.play(-4.0); // a 1-bar pre-count (PPQ −4 … 0)
    r.clock.preCount = true;
    r.step();
    CHECK(r.timing.patternStartPpq == 0.0);
    r.sw.request(0, 2);
    r.step();
    CHECK(r.last.switched);
    CHECK_FALSE(r.timing.boundary.has);
    CHECK(r.timing.patternStartPpq == 0.0);
    r.runUntilPpq(-0.01);
    r.clock.preCount = false;
    r.sw.request(0, 5); // queued in the block that crosses PPQ 0: lands on PPQ 0 exactly
    REQUIRE(r.runUntilSwitch(4));
    CHECK(r.timing.boundary.ppq == Catch::Approx(0.0).margin(1e-12));
    CHECK(r.lastSwitchFrame == frameOf(4.0, 120.0)); // frame 0 of the clock started at PPQ −4
}

TEST_CASE("scene.switch.resident: per-scene and per-pattern state is addressed by index, never shared", "[scene]")
{
    engine::scene::ScenePatternSwitcher sw;
    sw.setSceneBpm(3, 95.0f);
    sw.setPatternLength(3, 7, 40);
    sw.setPatternMuteSolo(3, 7, 0x00F0, 0x0001);
    CHECK(sw.sceneBpm(3) == 95.0f);
    CHECK(sw.sceneBpm(4) == 0.0f);
    CHECK(sw.patternLength(3, 7) == 40);
    CHECK(sw.patternLength(4, 7) == 16);
    CHECK(sw.patternMute(4, 7) == 0);
    CHECK(sw.patternSolo(4, 7) == 0);
    sw.setPatternLength(3, 7, 200);
    CHECK(sw.patternLength(3, 7) == 64);
    sw.request(16, 0); // out of range: ignored
    CHECK(sw.state().pendingScene == -1);
}
