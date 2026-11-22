// The sequencer against the switcher's boundary contract (engine/scene/ScenePatternSwitcher.h), inside
// engine::Engine with the real transport and switcher: the pass start moves at every wrap (no boundary), the block is
// split at timing.boundary.frame (old pattern before, new from it), RESTART / CONTINUE, a boundary on a block start,
// stopped / pre-count = immediate; plus the count-in (SEQ-044), pause / continue, the FX step locks (SEQ-026, FX-079),
// the ducker flag (SEQ-027) and MIDI recording (SEQ-048).
#include "tests/unit/sequencer/SeqRig.h"

#include "engine/fx/EffectRegistryOrder.h"
#include "engine/fx/FxSlotState.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace seqtest;
using core::model::TriggerType;

namespace
{
long floorFrame(double steps, double st) { return static_cast<long>(std::floor(steps * st)); }

// Every step of `track` holds a FULL trigger with velocity step + 1 (the hit names its pattern step).
std::unique_ptr<core::model::Pattern> everyStep(int track, int length = 16)
{
    auto p = pattern(length);
    for (int s = 0; s < length; ++s)
        put(*p, s, track, TriggerType::Full, s + 1);
    return p;
}

int switchFrame(const std::vector<engine::EngineEvent>& events)
{
    for (const auto& e : events)
        if (e.kind == engine::EngineEvent::Kind::SwitchDone)
            return e.step;
    return -1;
}
} // namespace

TEST_CASE("seq.boundary: the pass start moves at every wrap; a late offset across the wrap fires once (48 kHz)",
          "[seq][engine][boundary]")
{
    // 120 BPM at 48 kHz: a step is 6,000 frames, a 16-step pass 96,000 = 1,500 blocks, so every wrap is a block start
    // and the block after it already sees the new pass start: step 16 + 24/24 is "pass −1" of that block.
    Rig rig(false, 48000.0);
    auto p = pattern(16);
    put(*p, 15, 0);
    lock(*p, 15, 0, 15, 24.0f); // OFF SET +24/24 = the next pass's step 1 position
    put(*p, 0, 1);
    REQUIRE(rig.install(*p));
    REQUIRE(rig.play());
    rig.until(3 * 96000 + 64);
    const auto late = rig.sequenced(0);
    REQUIRE(late.size() == 3u); // nothing before the first pass; then once at every wrap
    CHECK(late[0].frame == 96000);
    CHECK(late[1].frame == 2 * 96000);
    CHECK(late[2].frame == 3 * 96000);
    const auto first = rig.sequenced(1);
    REQUIRE(first.size() == 4u);
    for (std::size_t i = 0; i < first.size(); ++i)
        CHECK(first[i].frame == static_cast<long>(i) * 96000);
}

TEST_CASE("seq.boundary: a PATTERN switch plays the old pattern before the boundary frame and the new one from it",
          "[seq][engine][boundary]")
{
    Rig rig;
    REQUIRE(rig.install(*everyStep(0), 0, 0));
    REQUIRE(rig.install(*everyStep(1), 0, 1));
    REQUIRE(rig.play());
    const double st = stepFrames();
    rig.until(floorFrame(4.5, st));
    REQUIRE(rig.queue(0, 1));
    rig.until(floorFrame(18.5, st));
    const auto old = rig.sequenced(0), next = rig.sequenced(1);
    REQUIRE(old.size() == 16u); // one full pass of the old pattern; its step 1 at the boundary never fires
    CHECK(old.back().frame == floorFrame(15, st));
    REQUIRE(next.size() == 3u);
    // RESTART: the new pattern's step 1 on the boundary (PPQ 4.0 = frame 88,200 = block 1,378 + 8).
    CHECK(next[0].frame == 88200);
    CHECK(next[0].velocity == 1);
    CHECK(next[1].frame == floorFrame(17, st));
    CHECK(switchFrame(rig.events()) == 88200 % 64);
}

TEST_CASE("seq.boundary: inside the boundary block, old events stop before the frame and the new pattern starts on it",
          "[seq][engine][boundary]")
{
    // STEP unit: the switch lands on step 23 (index 22, PPQ 5.5 = frame 121,275, block 1,894 + 59). Old step 22
    // (index 5 of the second pass, odd) with swing 49 % and OFF SET +12/24 sits 0.01 step (55 frames) earlier in the
    // same block: it plays; old step 23 (on the boundary) does not; the new pattern's step 1 does, on frame 59.
    Rig rig;
    auto a = pattern(16);
    put(*a, 5, 0);
    lock(*a, 5, 0, 15, 12.0f);
    put(*a, 6, 0, TriggerType::Full, 99);
    REQUIRE(rig.install(*a, 0, 0));
    REQUIRE(rig.install(*everyStep(1), 0, 1));
    REQUIRE(rig.param("seq.swing", 49.0f));
    REQUIRE(rig.param("project.patternQuantize", 0.0f)); // STEP
    REQUIRE(rig.play());
    const double st = stepFrames();
    rig.until(floorFrame(21.5, st));
    REQUIRE(rig.queue(0, 1));
    rig.until(floorFrame(24.0, st));
    const auto old = rig.sequenced(0), next = rig.sequenced(1);
    // Pass 1: steps 6 (+ swing + offset) and 7; pass 2: step 6 only.
    REQUIRE(old.size() == 3u);
    CHECK(old[2].frame == floorFrame(16 + 5 + 0.49 + 0.5, st));
    CHECK(old[2].frame / 64 == 121275 / 64); // the boundary block
    REQUIRE_FALSE(next.empty());
    CHECK(next[0].frame == 121275);
    CHECK(next[0].velocity == 1); // RESTART: step 1 of the new pattern
    CHECK(switchFrame(rig.events()) == 121275 % 64);
}

TEST_CASE("seq.boundary: CONTINUE keeps the step position; RESTART is the default", "[seq][engine][boundary]")
{
    Rig rig;
    REQUIRE(rig.install(*everyStep(0), 0, 0));
    REQUIRE(rig.install(*everyStep(1), 0, 1));
    REQUIRE(rig.param("project.patternQuantize", 0.0f)); // STEP
    REQUIRE(rig.param("scene.switchMode", 1.0f));        // CONTINUE
    REQUIRE(rig.play());
    const double st = stepFrames();
    rig.until(floorFrame(4.5, st));
    REQUIRE(rig.queue(0, 1));
    rig.until(floorFrame(7.5, st));
    const auto old = rig.sequenced(0), next = rig.sequenced(1);
    REQUIRE(old.size() == 5u); // steps 1–5 of the old pattern
    REQUIRE(next.size() == 3u);
    CHECK(next[0].frame == floorFrame(5, st));
    CHECK(next[0].velocity == 6); // step 6 of the new pattern, on the boundary
    CHECK(next[2].velocity == 8);
}

TEST_CASE("seq.boundary: a switch while stopped is immediate; PLAY starts the new pattern on step 1, once",
          "[seq][engine][boundary]")
{
    Rig rig;
    REQUIRE(rig.install(*everyStep(0), 0, 0));
    REQUIRE(rig.install(*everyStep(1), 0, 1));
    rig.blocks(2);
    REQUIRE(rig.queue(0, 1));
    rig.blocks(2);
    REQUIRE(rig.play());
    const long start = rig.frame;
    rig.until(start + floorFrame(2.5, stepFrames()));
    CHECK(rig.sequenced(0).empty());
    const auto next = rig.sequenced(1);
    REQUIRE(next.size() == 3u);
    CHECK(next[0].frame == start);
    CHECK(next[0].velocity == 1);
}

TEST_CASE("seq.boundary: a boundary on a block start plays nothing of the old pattern in that block (48 kHz)",
          "[seq][engine][boundary]")
{
    Rig rig(false, 48000.0);
    REQUIRE(rig.install(*everyStep(0), 0, 0));
    REQUIRE(rig.install(*everyStep(1), 0, 1));
    REQUIRE(rig.play());
    rig.until(5 * 6000);
    REQUIRE(rig.queue(0, 1));
    rig.until(96000 + 2 * 6000 + 64);
    const auto old = rig.sequenced(0), next = rig.sequenced(1);
    REQUIRE(old.size() == 16u);
    CHECK(old.back().frame == 15 * 6000);
    REQUIRE(next.size() == 3u);
    CHECK(next[0].frame == 96000);
    CHECK(switchFrame(rig.events()) == 0);
}

TEST_CASE("seq.boundary: the pre-count plays no pattern; a switch in it is immediate; step 1 on the start frame",
          "[seq][engine][boundary][record]")
{
    Rig rig;
    REQUIRE(rig.install(*everyStep(0), 0, 0));
    auto b = pattern(16);
    put(*b, 0, 1);
    REQUIRE(rig.install(*b, 0, 1));
    REQUIRE(rig.param("seq.record", 1.0f)); // REC armed, 1/16 (default)
    REQUIRE(rig.play(1));                   // 1 bar = 4 quarters = 88,200 frames at 120 BPM / 44.1 kHz
    rig.until(20000);
    REQUIRE(rig.queue(0, 1)); // counting in: immediate
    rig.block();
    CHECK(switchFrame(rig.events()) == 0);
    const double st = stepFrames();
    // A pad hit 0.7 step before the start is nearer the count-in's last 16th: not recorded. One 0.2 step before is
    // recorded onto step 1 (SEQ-044, our choice) and, having sounded, does not sound again on step 1 of the first pass.
    rig.until(88200 - floorFrame(0.7, st) - 64);
    REQUIRE(rig.e->triggers().push({4, 100, 60, 4}));
    rig.block();
    CHECK(rig.events().empty());
    rig.until(88200 - floorFrame(0.2, st) - 64);
    REQUIRE(rig.e->triggers().push({5, 110, 60, 5}));
    rig.block();
    auto ev = rig.events();
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].kind == engine::EngineEvent::Kind::RecordedHit);
    CHECK(ev[0].track == 5);
    CHECK(ev[0].step == 0);
    CHECK(ev[0].offset24 == 0);
    put(*b, 0, 5, TriggerType::Full, 110); // what recordHit + the pattern sync install before the start
    REQUIRE(rig.install(*b, 0, 1));
    rig.until(88200 + floorFrame(16.5, st));
    CHECK(rig.sequenced(0).empty());
    const auto played = rig.sequenced(1);
    REQUIRE(played.size() == 2u); // step 1 of passes 1 and 2
    CHECK(played[0].frame == 88200);
    CHECK(played[1].frame == 88200 + 88200);
    const auto rec = rig.sequenced(5);
    REQUIRE(rec.size() == 1u); // not in pass 1 (it sounded live), in pass 2
    CHECK(rec[0].frame == 88200 + 88200);
}

TEST_CASE("seq.boundary: pause and CONTINUE keep the pattern phase", "[seq][engine][boundary]")
{
    Rig rig;
    REQUIRE(rig.install(*everyStep(0)));
    REQUIRE(rig.play());
    const double st = stepFrames();
    rig.until(floorFrame(5.3, st));
    REQUIRE(rig.pause());
    const long paused = rig.frame; // the block that drains the pause freezes the position at its start
    rig.blocks(40);
    REQUIRE(rig.resume());
    const long resumed = rig.frame;
    rig.until(resumed + floorFrame(1.0, st));
    const auto hits = rig.sequenced(0);
    REQUIRE(hits.size() == 7u); // steps 1–6, then 7 after CONTINUE; none twice
    CHECK(hits[6].velocity == 7);
    const double ppf = 120.0 / (60.0 * 44100.0);
    const double resumePpq = paused * ppf;
    CHECK(hits[6].frame == resumed + static_cast<long>(std::floor((6 * 0.25 - resumePpq) / ppf + 1e-6)));
}

TEST_CASE("seq.engine: every sequencer trigger sends the track's FX1 the six values, locked or stored (SEQ-026)",
          "[seq][engine][fx]")
{
    Rig rig(Rig::Full {});
    const auto list = engine::fx::engineTypes(engine::fx::EffectRegistry::builtin(), engine::fx::SlotKind::Insert);
    const int type = engine::fx::typeIndexOf(list, "gainTest");
    REQUIRE(type >= 0);
    REQUIRE(rig.param("fx1.gainTest.gain", 0.5f, 0x00));
    REQUIRE(rig.param("fx1.type", static_cast<float>(type + 1), 0x00)); // scene 1, track 1
    auto p = pattern(16);
    put(*p, 0, 0);
    lock(*p, 0, 0, 23, 0.25f); // FX1 k0
    put(*p, 2, 0, TriggerType::LockOnly);
    REQUIRE(rig.install(*p));
    REQUIRE(rig.play());
    const double st = stepFrames();
    const int slot = engine::fx::insertSlot(0, 0);
    rig.until(floorFrame(1.0, st));
    CHECK(rig.e->fxBank().effective(slot, 0) == Catch::Approx(0.25f));
    CHECK(rig.e->fxBank().lockMask(slot) == 1u);
    rig.until(floorFrame(2.5, st)); // PARAM step 3 without an FX lock: the stored value snaps back (FX-079)
    CHECK(rig.e->fxBank().effective(slot, 0) == Catch::Approx(0.5f));
    CHECK(rig.e->fxBank().lockMask(slot) == 0u);
}

TEST_CASE("seq.engine: FULL and HALF steps duck, PARAM steps do not (SEQ-027); MIDI notes are recorded (SEQ-048)",
          "[seq][engine][record]")
{
    Rig rig;
    auto p = pattern(16);
    put(*p, 0, 0, TriggerType::Full);
    put(*p, 1, 0, TriggerType::Half);
    put(*p, 2, 0, TriggerType::LockOnly);
    REQUIRE(rig.install(*p));
    REQUIRE(rig.param("seq.record", 1.0f));
    REQUIRE(rig.play());
    const double st = stepFrames();
    rig.until(floorFrame(3.0, st));
    const auto hits = rig.sequenced(0);
    REQUIRE(hits.size() == 3u);
    CHECK(hits[0].ducks);
    CHECK(hits[1].ducks);
    CHECK_FALSE(hits[2].ducks);
    // A MIDI note (no pad) for track 8 while recording.
    rig.until(floorFrame(6.0, st) - 64);
    REQUIRE(rig.e->triggers().push({7, 90, 60, -1}));
    rig.block();
    const auto ev = rig.events();
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].kind == engine::EngineEvent::Kind::RecordedHit);
    CHECK(ev[0].track == 7);
    CHECK(ev[0].step == 6);
    CHECK(ev[0].value == 90);
}

TEST_CASE("seq.boundary: a locate backwards keeps the pass grid; the located step fires on the block start, once",
          "[seq][engine][boundary]")
{
    Rig rig;
    REQUIRE(rig.install(*everyStep(0)));
    REQUIRE(rig.play());
    const double st = stepFrames();
    rig.until(floorFrame(37.5, st)); // pass 3, step 6
    const std::size_t before = rig.sequenced(0).size();
    CHECK(before == 38u);
    REQUIRE(rig.locate(1.0)); // PPQ 1.0 = step 5 of pass 1 (the switcher steps back whole passes)
    const long located = rig.frame;
    rig.until(located + floorFrame(1.5, st));
    const auto hits = rig.sequenced(0);
    REQUIRE(hits.size() == before + 2u);
    CHECK(hits[before].frame == located);
    CHECK(hits[before].velocity == 5);
    CHECK(hits[before + 1].frame == located + floorFrame(1.0, st));
    CHECK(hits[before + 1].velocity == 6);
}
