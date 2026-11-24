// The step sequencer inside engine::Engine (step 6): sample-accurate step times at
// 120 BPM / 44.1 kHz, every step exactly once including step 1 (SEQ-014), swing (SEQ-004), offsets (SEQ-005), locks
// with Revert / Hold (SEQ-021), HALF and PARAM (SEQ-011), the switch boundary split, and real-time recording
// (SEQ-041/042/046/047): quantised positions, "recorded this pass", erase, pad starts sequence.
#include "tests/unit/sequencer/SeqRig.h"

#include "core/util/ribbon/Ribbon.h"
#include "engine/trigger/TrackParams.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace seqtest;
using core::model::LockChange;
using core::model::TriggerType;

TEST_CASE("seq.engine: a 16-step FULL pattern at 120 BPM / 44.1 kHz triggers at n x 5512.5 frames, each step once",
          "[seq][engine]")
{
    Rig rig;
    auto p = pattern(16);
    for (int s = 0; s < 16; ++s)
        put(*p, s, 0);
    REQUIRE(rig.install(*p));
    REQUIRE(rig.play());
    rig.until(static_cast<long>(3 * 16 * stepFrames())); // three passes
    const auto hits = rig.sequenced(0);
    REQUIRE(hits.size() == 48u + 1u); // step 1 of the fourth pass falls in the last rendered block
    for (std::size_t n = 0; n < hits.size(); ++n)
    {
        INFO("step " << n);
        CHECK(hits[n].frame == static_cast<long>(std::floor(static_cast<double>(n) * 5512.5)));
        CHECK(hits[n].kind == TriggerKind::Full);
        CHECK(hits[n].velocity == 127);
    }
    CHECK(hits[0].frame == 0); // step 1 on start, once
    CHECK(rig.e->voiceStarts() == 0); // no sample: the router applied them, nothing sounded
}

TEST_CASE("seq.engine: swing 50 % delays the even steps by exactly half a step", "[seq][engine]")
{
    Rig rig;
    auto p = pattern(16);
    for (int s = 0; s < 4; ++s)
        put(*p, s, 2);
    REQUIRE(rig.install(*p));
    REQUIRE(rig.param("seq.swing", 50.0f));
    REQUIRE(rig.play());
    rig.until(static_cast<long>(4 * stepFrames()));
    const auto hits = rig.sequenced(2);
    REQUIRE(hits.size() == 4u);
    const double step = stepFrames();
    CHECK(hits[0].frame == 0);
    CHECK(hits[1].frame == static_cast<long>(std::floor(1.5 * step))); // 2nd step + half a step
    CHECK(hits[2].frame == static_cast<long>(std::floor(2.0 * step)));
    CHECK(hits[3].frame == static_cast<long>(std::floor(3.5 * step)));
}

TEST_CASE("seq.engine: an offset of -24/24 on step 1 fires at the end of the previous pass", "[seq][engine]")
{
    Rig rig;
    auto p = pattern(16);
    put(*p, 0, 1);
    lock(*p, 0, 1, 15, -24.0f); // OFF SET lock (slot 15)
    put(*p, 8, 1);
    REQUIRE(rig.install(*p));
    REQUIRE(rig.play());
    rig.until(static_cast<long>(33 * stepFrames()));
    const auto hits = rig.sequenced(1);
    // Pass 1: no step 1 before the transport started; step 9; then step 1 of pass 2 one step early, …
    REQUIRE(hits.size() == 4u);
    CHECK(hits[0].frame == static_cast<long>(std::floor(8 * stepFrames())));
    CHECK(hits[1].frame == static_cast<long>(std::floor(15 * stepFrames())));
    CHECK(hits[2].frame == static_cast<long>(std::floor(24 * stepFrames())));
    CHECK(hits[3].frame == static_cast<long>(std::floor(31 * stepFrames())));
    // The track offset applies to steps without a lock: +12/24 moves step 9 by half a step.
    Rig rig2;
    REQUIRE(rig2.install(*p));
    REQUIRE(rig2.param("seq.offset", 12.0f, 0x01)); // scene 0, track 2
    REQUIRE(rig2.play());
    rig2.until(static_cast<long>(9 * stepFrames()));
    const auto h2 = rig2.sequenced(1);
    REQUIRE(h2.size() == 1u);
    CHECK(h2[0].frame == static_cast<long>(std::floor(8.5 * stepFrames())));
}

namespace
{
// A linear ramp, long enough for 16 steps: the voice position shows the playback rate (pitch).
voicetest::TestBuffer& ramp()
{
    static voicetest::TestBuffer b([] {
        std::vector<float> v(400000);
        for (std::size_t i = 0; i < v.size(); ++i)
            v[i] = 0.1f;
        return v;
    }());
    return b;
}

bool installSample(Rig& rig, int track)
{
    core::audio::retain(ramp().get());
    return rig.e->bridge().post(engine::ObjectSwap {engine::voice::trackSampleSlot(0, track), ramp().get()});
}

// Voice frames advanced per output frame over [from, to) (absolute frames, block-aligned reads).
double rate(Rig& rig, int track, long from, long to)
{
    rig.until(from);
    const auto a = rig.e->trackSource(track).voice().position();
    const long fa = rig.frame;
    rig.until(to);
    const auto b = rig.e->trackSource(track).voice().position();
    return static_cast<double>(b - a) / static_cast<double>(rig.frame - fa);
}
} // namespace

TEST_CASE("seq.engine: a pitch lock on step 3 applies on step 3 and reverts on step 4; Hold keeps it", "[seq][engine]")
{
    for (const auto change : {LockChange::Revert, LockChange::Hold})
    {
        INFO((change == LockChange::Hold ? "Hold" : "Revert"));
        Rig rig;
        REQUIRE(installSample(rig, 0));
        auto p = pattern(16);
        for (int s = 0; s < 5; ++s)
            put(*p, s, 0);
        lock(*p, 2, 0, 4, 12.0f, change); // PITCH +12 semitones (playback.pitch units) on step 3
        REQUIRE(rig.install(*p));
        REQUIRE(rig.play());
        const double st = stepFrames();
        CHECK(rate(rig, 0, static_cast<long>(1.2 * st), static_cast<long>(1.8 * st)) == Catch::Approx(1.0).margin(0.02));
        CHECK(rate(rig, 0, static_cast<long>(2.2 * st), static_cast<long>(2.8 * st)) == Catch::Approx(2.0).margin(0.02));
        const double after = rate(rig, 0, static_cast<long>(3.2 * st), static_cast<long>(3.8 * st));
        CHECK(after == Catch::Approx(change == LockChange::Hold ? 2.0 : 1.0).margin(0.02));
        CHECK(rig.e->voiceStarts() >= 4);
    }
}

TEST_CASE("seq.engine: HALF continues the sounding voice, PARAM changes only the locked parameters", "[seq][engine]")
{
    Rig rig;
    REQUIRE(installSample(rig, 0));
    auto p = pattern(16);
    put(*p, 0, 0);                         // FULL
    put(*p, 2, 0, TriggerType::Half);      // HALF: the voice continues
    put(*p, 4, 0, TriggerType::LockOnly);  // PARAM with a pitch lock
    lock(*p, 4, 0, 4, 12.0f);
    put(*p, 8, 1, TriggerType::Half);      // HALF on a silent track: nothing starts
    REQUIRE(rig.install(*p));
    REQUIRE(rig.play());
    const double st = stepFrames();
    rig.until(static_cast<long>(1.9 * st));
    const auto before = rig.e->trackSource(0).voice().position();
    rig.until(static_cast<long>(2.1 * st));
    CHECK(rig.e->trackSource(0).voice().position() > before); // not restarted at step 3
    CHECK(rig.e->voiceStarts() == 1);
    CHECK(rate(rig, 0, static_cast<long>(3.2 * st), static_cast<long>(3.8 * st)) == Catch::Approx(1.0).margin(0.02));
    CHECK(rate(rig, 0, static_cast<long>(4.2 * st), static_cast<long>(4.8 * st)) == Catch::Approx(2.0).margin(0.02));
    CHECK(rig.e->voiceStarts() == 1); // PARAM started nothing
    rig.until(static_cast<long>(8.5 * st));
    CHECK(rig.e->trackSource(1).voice().position() < 0); // HALF with no voice sounding: nothing starts (SEQ-011)
    const auto hits = rig.sequenced();
    REQUIRE(hits.size() == 4u);
    CHECK(hits[1].kind == TriggerKind::Half);
    CHECK(hits[2].kind == TriggerKind::LockOnly);
    CHECK(hits[2].locked);
}

TEST_CASE("seq.engine: real-time hits at 1/16T land on step + offset 16/24; a recorded step does not sound twice",
          "[seq][engine][record]")
{
    Rig rig;
    auto p = pattern(16);
    put(*p, 0, 3); // something already on track 4 (overdub)
    REQUIRE(rig.install(*p));
    REQUIRE(rig.param("seq.record", 1.0f));
    REQUIRE(rig.param("seq.recordQuantize", 2.0f)); // 1/16T
    REQUIRE(rig.play());
    const double st = stepFrames();
    // A pad hit 2/3 of a step in (the second 16th triplet of the beat): the TriggerSink delivers it at the next block.
    rig.until(static_cast<long>(0.66 * st) - 64);
    REQUIRE(rig.e->triggers().push({5, 100, 60, 5}));
    rig.block();
    auto ev = rig.events();
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].kind == engine::EngineEvent::Kind::RecordedHit);
    CHECK(ev[0].track == 5);
    CHECK(ev[0].step == 0);
    CHECK(ev[0].offset24 == 16);
    CHECK(ev[0].value == 100);

    // 1/16: a hit just before step 5 is stored on step 5 — it sounded live, so step 5 does not sound again this pass,
    // even after the pattern object with the new trigger arrived.
    REQUIRE(rig.param("seq.recordQuantize", 3.0f));
    rig.until(static_cast<long>(3.9 * st));
    REQUIRE(rig.e->triggers().push({5, 90, 60, 5}));
    rig.block();
    ev = rig.events();
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].step == 4);
    CHECK(ev[0].offset24 == 0);
    CHECK((rig.e->triggerRouter().blockEvents().size() == 1)); // the live hit sounded
    put(*p, 0, 5);
    lock(*p, 0, 5, 15, 16.0f);
    put(*p, 4, 5, TriggerType::Full, 90);
    REQUIRE(rig.install(*p)); // what the message thread's recordHit + rebuild installs
    rig.until(static_cast<long>(16 * st) - 64);
    CHECK(rig.sequenced(5).empty()); // neither step sounded a second time in this pass
    rig.until(static_cast<long>(21 * st));
    const auto next = rig.sequenced(5);
    REQUIRE(next.size() == 2u); // the next pass plays both
    CHECK(next[0].frame == static_cast<long>(std::floor((16 + 16.0 / 24.0) * st)));
    CHECK(next[1].frame == static_cast<long>(std::floor(20 * st)));
    CHECK(rig.sequenced(3).size() == 2u); // overdub kept track 4
}

TEST_CASE("seq.engine: a recorded MIDI note carries its semitones; a PLAY pad hit is unpitched",
          "[seq][engine][record]")
{
    Rig rig;
    auto p = pattern(16);
    REQUIRE(rig.install(*p));
    REQUIRE(rig.param("seq.record", 1.0f));
    REQUIRE(rig.play());
    const double st = stepFrames();
    rig.until(static_cast<long>(2.0 * st));
    REQUIRE(rig.e->triggers().push({5, 100, 67, -1})); // a MIDI note 7 semitones up on track 6
    rig.block();
    auto ev = rig.events();
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].kind == engine::EngineEvent::Kind::RecordedHit);
    CHECK(ev[0].track == 5);
    CHECK(ev[0].pitched);
    CHECK(ev[0].semitones == 7);
    rig.until(static_cast<long>(4.0 * st));
    REQUIRE(rig.e->triggers().push({5, 100, 60, 5})); // pad 6 in PLAY mode
    rig.block();
    ev = rig.events();
    REQUIRE(ev.size() == 1u);
    CHECK_FALSE(ev[0].pitched);
    CHECK(ev[0].semitones == 0);
}

TEST_CASE("seq.engine: quantize OFF stores the exact timing; nothing is recorded while not armed", "[seq][engine][record]")
{
    Rig rig;
    auto p = pattern(16);
    REQUIRE(rig.install(*p));
    REQUIRE(rig.play());
    const double st = stepFrames();
    rig.until(static_cast<long>(2.0 * st));
    REQUIRE(rig.e->triggers().push({0, 100, 60, 0}));
    rig.block();
    CHECK(rig.events().empty()); // REC off
    REQUIRE(rig.param("seq.record", 1.0f));
    REQUIRE(rig.param("seq.recordQuantize", 0.0f));
    rig.until(static_cast<long>(6.25 * st) - 64);
    REQUIRE(rig.e->triggers().push({0, 100, 60, 0}));
    rig.block();
    const auto ev = rig.events();
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].step == 6);
    CHECK(ev[0].offset24 == 6); // 1/4 step late, in 1/24 steps
}

TEST_CASE("seq.engine: ERASE held removes the track's triggers as the playhead passes", "[seq][engine][record]")
{
    Rig rig;
    auto p = pattern(16);
    for (int s = 0; s < 16; s += 2)
        put(*p, s, 7);
    REQUIRE(rig.install(*p));
    REQUIRE(rig.play());
    const double st = stepFrames();
    rig.until(static_cast<long>(3.5 * st));
    REQUIRE(rig.param("seq.erase", 1.0f, 7));
    rig.until(static_cast<long>(8.5 * st));
    REQUIRE(rig.param("seq.erase", 0.0f, 7));
    rig.until(static_cast<long>(15.5 * st));
    const auto ev = rig.events();
    REQUIRE(ev.size() == 3u); // steps 5, 7, 9
    for (std::size_t i = 0; i < ev.size(); ++i)
    {
        CHECK(ev[i].kind == engine::EngineEvent::Kind::ErasedStep);
        CHECK(ev[i].track == 7);
        CHECK(ev[i].step == 4 + 2 * i);
    }
    CHECK(rig.sequenced(7).size() == 8u - 3u); // the erased steps were not played
}

TEST_CASE("seq.engine: PAD STARTS SEQ — the first hit while stopped and armed starts the transport on step 1",
          "[seq][engine][record]")
{
    Rig rig;
    auto p = pattern(16);
    put(*p, 4, 2);
    REQUIRE(rig.install(*p));
    REQUIRE(rig.param("seq.record", 1.0f));
    REQUIRE(rig.param("seq.padStartsSequence", 1.0f));
    rig.blocks(4);
    CHECK_FALSE(rig.e->bridge().playhead().playing);
    REQUIRE(rig.e->triggers().push({2, 110, 60, 2}));
    rig.block();
    const auto ev = rig.events();
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].step == 0);
    CHECK(ev[0].offset24 == 0);
    rig.blocks(2);
    CHECK(rig.e->bridge().playhead().playing);
    rig.until(static_cast<long>(6 * stepFrames()));
    const auto hits = rig.sequenced(2);
    REQUIRE(hits.size() == 1u); // step 5 plays; step 1 (just recorded, sounded live) does not sound twice
    CHECK(hits[0].frame == 5 * 64 + static_cast<long>(std::floor(4 * stepFrames())));
}

TEST_CASE("seq.engine: PAD STARTS SEQ under MIDI clock — blocked (nothing recorded) while only the clock's Start "
          "starts the transport",
          "[seq][engine][record]")
{
    Rig rig;
    auto p = pattern(16);
    put(*p, 4, 2);
    REQUIRE(rig.install(*p));
    REQUIRE(rig.param("seq.record", 1.0f));
    REQUIRE(rig.param("seq.padStartsSequence", 1.0f));
    REQUIRE(rig.param("midi.syncSource", 1.0f)); // MIDI CLOCK, IN CLOCK and START/STOP/CONTINUE on (defaults)
    rig.blocks(4);
    REQUIRE(rig.e->bridge().playhead().externalStart);
    REQUIRE(rig.e->triggers().push({2, 110, 60, 2}));
    rig.block();
    CHECK(rig.events().empty()); // no step-1 hit recorded: no pass starts from it
    rig.blocks(4);
    CHECK_FALSE(rig.e->bridge().playhead().playing);
    // IN START/STOP/CONTINUE off (manual p.45: the master cannot start the unit): the pad starts it, as without sync.
    REQUIRE(rig.param("midi.in.startStop", 0.0f));
    rig.blocks(2);
    REQUIRE_FALSE(rig.e->bridge().playhead().externalStart);
    REQUIRE(rig.e->triggers().push({2, 110, 60, 2}));
    rig.block();
    const auto ev = rig.events();
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].step == 0);
    rig.blocks(2);
    CHECK(rig.e->bridge().playhead().playing);
}

TEST_CASE("seq.engine: an 8-step pattern wraps after 8 steps; STOP rewinds to step 1", "[seq][engine]")
{
    Rig rig;
    auto p = pattern(8);
    put(*p, 0, 0);
    REQUIRE(rig.install(*p));
    REQUIRE(rig.play());
    rig.until(static_cast<long>(9.5 * stepFrames()));
    auto hits = rig.sequenced(0);
    REQUIRE(hits.size() == 2u);
    CHECK(hits[1].frame == static_cast<long>(std::floor(8 * stepFrames())));
    REQUIRE(rig.stop());
    rig.blocks(4);
    REQUIRE(rig.play()); // from stop: back to step 1, which fires once on the new start (SEQ-014)
    const long start = rig.frame;
    rig.until(start + 640);
    hits = rig.sequenced(0);
    REQUIRE(hits.size() == 3u);
    CHECK(hits[2].frame == start);
}

namespace
{
// The touch strip as the app sends it (core/util/ribbon/Ribbon.h): position 0–127, contact.
engine::TriggerNow strip(int position, bool contact)
{
    return {0, static_cast<std::uint8_t>(position), static_cast<std::uint8_t>(contact ? 1 : 0),
            core::ribbon::kStripPad};
}

int repeatsRecorded(const std::vector<engine::EngineEvent>& ev, int track)
{
    int n = 0;
    for (const auto& e : ev)
        n += e.kind == engine::EngineEvent::Kind::RecordedHit && e.origin == engine::EngineEvent::kOriginRepeat
                     && e.track == track
                 ? 1
                 : 0;
    return n;
}
} // namespace

TEST_CASE("seq.engine: touch strip REPEAT hits are recorded on the steps while REC is armed and playing (RIB-008)",
          "[seq][engine][record][ribbon]")
{
    Rig rig;
    auto p = pattern(16);
    REQUIRE(rig.install(*p));
    REQUIRE(rig.param("ribbon.mode", 1.0f)); // REPEAT
    REQUIRE(rig.param("seq.recordQuantize", 3.0f)); // 1/16
    REQUIRE(rig.param("seq.record", 1.0f));
    REQUIRE(rig.play());
    const double st = stepFrames();
    rig.until(static_cast<long>(2.0 * st) - 64);
    REQUIRE(rig.e->triggers().push({3, 100, 60, 3})); // pad 4 held: the pad hit itself is recorded (origin live)
    REQUIRE(rig.e->triggers().push(strip(80, true))); // the 1/16 zone
    rig.block();
    auto ev = rig.events();
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].origin == engine::EngineEvent::kOriginLive);
    rig.until(static_cast<long>(8.0 * st) - 64);
    ev = rig.events();
    REQUIRE(repeatsRecorded(ev, 3) == 5); // steps 4…8: one repeat per 16th, each on its own step
    for (int i = 0; i < 5; ++i)
    {
        INFO("repeat " << i);
        CHECK(ev[static_cast<std::size_t>(i)].step == 3 + i);
        CHECK(ev[static_cast<std::size_t>(i)].offset24 == 0);
        CHECK(ev[static_cast<std::size_t>(i)].value == 100);
    }
    // REC off: the repeats still sound but nothing is recorded.
    REQUIRE(rig.param("seq.record", 0.0f));
    const auto sounded = rig.hits.size();
    rig.until(static_cast<long>(12.0 * st));
    CHECK(rig.events().empty());
    CHECK(rig.hits.size() >= sounded + 3);
}

TEST_CASE("seq.engine: the REPEAT of a held SCALE pad is recorded at the pad's pitch; the root's drops the lock",
          "[seq][engine][record][ribbon]")
{
    Rig rig;
    auto p = pattern(16);
    REQUIRE(rig.install(*p));
    REQUIRE(rig.param("ribbon.mode", 1.0f));        // REPEAT
    REQUIRE(rig.param("seq.recordQuantize", 3.0f)); // 1/16
    REQUIRE(rig.param("pads.selected", 3.0f, 0xFFFF));
    REQUIRE(rig.param("pads.mode", static_cast<float>(engine::trigger::PadMode::Scale), 0xFFFF));
    REQUIRE(rig.param("seq.record", 1.0f));
    REQUIRE(rig.play());
    const double st = stepFrames();
    rig.until(static_cast<long>(2.0 * st) - 64);
    REQUIRE(rig.e->triggers().push({7, 100, 60, 7})); // SCALE pad 8 held: 7 semitones (chromatic) on track 4
    REQUIRE(rig.e->triggers().push(strip(80, true)));
    rig.block();
    rig.until(static_cast<long>(6.5 * st));
    auto ev = rig.events();
    REQUIRE(repeatsRecorded(ev, 3) >= 3);
    for (const auto& e : ev)
    {
        CHECK(e.track == 3);
        CHECK(e.pitched); // the pad hit and its repeats: the key played
        CHECK(e.semitones == 7);
    }
    // The root pad's repeats are pitched too, at 0 semitones: the recorder drops an older PITCH lock there.
    REQUIRE(rig.e->triggers().push(engine::TriggerNow{7, 0, 60, 7})); // pad 8 up
    REQUIRE(rig.e->triggers().push({0, 100, 60, 0}));                 // pad 1 (the root) held
    rig.until(static_cast<long>(10.0 * st) - 64);
    ev = rig.events();
    REQUIRE(repeatsRecorded(ev, 3) >= 2);
    for (const auto& e : ev)
    {
        CHECK(e.pitched);
        CHECK(e.semitones == 0);
    }
}

TEST_CASE("seq.engine: REPEAT hits never start the transport or fill a count-in", "[seq][engine][record][ribbon]")
{
    Rig rig;
    auto p = pattern(16);
    REQUIRE(rig.install(*p));
    REQUIRE(rig.param("ribbon.mode", 1.0f));
    REQUIRE(rig.e->triggers().push({2, 90, 60, 2})); // held before REC is armed: it starts nothing
    REQUIRE(rig.e->triggers().push(strip(127, true))); // 1/32, free-running while stopped
    rig.blocks(4);
    REQUIRE(rig.param("seq.record", 1.0f));
    rig.until(static_cast<long>(8.0 * stepFrames()));
    int repeats = 0;
    for (const auto& h : rig.hits)
        repeats += h.source == TriggerSource::Repeat ? 1 : 0;
    CHECK(repeats >= 10); // it plays
    CHECK(repeatsRecorded(rig.events(), 2) == 0);
    CHECK_FALSE(rig.e->bridge().playhead().playing);
    // A one-bar count-in: nothing is recorded until step 1 of the pass.
    REQUIRE(rig.play(1));
    rig.until(rig.frame + static_cast<long>(15.5 * stepFrames()));
    CHECK(repeatsRecorded(rig.events(), 2) == 0);
    rig.until(rig.frame + static_cast<long>(2.0 * stepFrames()));
    CHECK(repeatsRecorded(rig.events(), 2) > 0);
}
