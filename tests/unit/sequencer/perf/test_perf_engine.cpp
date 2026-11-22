// Ratchets and transpose inside engine::Engine (spec 02-features/sequencer.md SEQ-030 / 035,
// amp-envelope.md ENV-007, OQ-09): offline block-by-block renders place every repeat at the exact frame (counts 1–16 ×
// six speeds × swing 0 / 50 % × 60 / 120 / 300 BPM, with offsets), the audio onsets follow, a new step / a choke /
// STOP cancel pending repeats, repeats keep the step's locks, the envelope restarts per ENV-007, and transpose moves
// the voice by exactly its semitones.
#include "tests/unit/sequencer/SeqRig.h"

#include "engine/sequencer/perf/PerfStage.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace seqtest;
using core::model::TriggerType;
namespace perf = engine::sequencer::perf;

namespace
{
std::uint16_t scope(int track, int scene = 0) { return static_cast<std::uint16_t>((scene << 4) | track); }

std::vector<Hit> repeats(const Rig& rig, int track)
{
    std::vector<Hit> v;
    for (const auto& h : rig.hits)
        if (h.source == TriggerSource::Retrigger && h.track == track)
            v.push_back(h);
    return v;
}

// t0 + k · step / divisor rounded half up, in integers: 2 · step is a whole number of frames at 60 / 120 / 300 BPM.
long expectedRepeat(long t0, int k, int speed, double bpm)
{
    const auto step2 = static_cast<long>(2.0 * perf::stepSamplesAt(44100.0, bpm));
    REQUIRE(static_cast<double>(step2) == 2.0 * perf::stepSamplesAt(44100.0, bpm));
    const long d = perf::retrigDivisor(speed);
    return t0 + (k * step2 + d) / (2 * d);
}

// A constant sample (the voice position shows the playback rate) and a one-frame impulse (audio onsets).
voicetest::TestBuffer& dc()
{
    static voicetest::TestBuffer b(std::vector<float>(400000, 0.1f));
    return b;
}
voicetest::TestBuffer& impulse()
{
    static voicetest::TestBuffer b([] {
        std::vector<float> v(64, 0.0f);
        v[0] = 1.0f;
        return v;
    }());
    return b;
}
bool installSample(Rig& rig, int track, voicetest::TestBuffer& b)
{
    core::audio::retain(b.get());
    return rig.e->bridge().post(engine::ObjectSwap {engine::voice::trackSampleSlot(0, track), b.get()});
}
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

TEST_CASE("perf.engine: every ratchet onset at the exact frame, counts 1-16 x 6 speeds x swing 0/50 % x 60/120/300 BPM",
          "[perf][engine]")
{
    for (const float swing : {0.0f, 50.0f})
        for (const double bpm : {60.0, 120.0, 300.0})
            for (int speed = 0; speed < perf::kRetrigSpeeds; ++speed)
            {
                INFO("swing " << swing << " bpm " << bpm << " speed " << speed);
                Rig rig;
                auto p = pattern(32); // one pass holds the step and all its repeats
                for (int t = 0; t < 16; ++t)
                {
                    put(*p, 1, t, TriggerType::Full, 100); // the 2nd step: swung
                    lock(*p, 1, t, perf::kLockSlotRetrigSpeed, static_cast<float>(speed));
                    REQUIRE(rig.param("seq.retrigCount", static_cast<float>(t + 1), scope(t))); // the track's count
                    if ((t & 1) != 0)
                        REQUIRE(rig.param("seq.offset", 6.0f, scope(t))); // a later start (track offset)
                }
                lock(*p, 1, 14, 15, -12.0f); // an OFF SET lock: earlier
                lock(*p, 1, 15, perf::kLockSlotRetrigCount, 7.0f); // a count lock overrides the track's 16
                REQUIRE(rig.install(*p));
                REQUIRE(rig.param("pattern.length", 32.0f, 0)); // the switcher's pass length (scene 1 pattern 1)
                REQUIRE(rig.param("seq.swing", swing));
                REQUIRE(rig.param("transport.tempo", static_cast<float>(bpm)));
                REQUIRE(rig.play());
                rig.until(static_cast<long>(17.4 * perf::stepSamplesAt(44100.0, bpm)));
                long first[16] = {};
                for (int t = 0; t < 16; ++t)
                {
                    INFO("track " << t);
                    const auto steps = rig.sequenced(t);
                    REQUIRE(steps.size() == 1u);
                    first[t] = steps[0].frame;
                    const int count = t == 15 ? 7 : t + 1;
                    const auto r = repeats(rig, t);
                    REQUIRE(r.size() == static_cast<std::size_t>(count - 1));
                    for (int k = 1; k < count; ++k)
                    {
                        CHECK(r[static_cast<std::size_t>(k - 1)].frame == expectedRepeat(steps[0].frame, k, speed, bpm));
                        CHECK(r[static_cast<std::size_t>(k - 1)].kind == TriggerKind::Full);
                        CHECK(r[static_cast<std::size_t>(k - 1)].velocity == 100);
                        CHECK(r[static_cast<std::size_t>(k - 1)].locked); // the step's locks, copied
                    }
                }
                // The offsets did move the starts the repeats are measured from.
                CHECK(first[1] > first[0]);
                CHECK(first[14] < first[0]);
                CHECK(rig.e->triggerRouter().dropped() == 0);
            }
}

TEST_CASE("perf.engine: the ratchets sound - audio onsets of an impulse sample at the step and every repeat",
          "[perf][engine]")
{
    Rig rig;
    REQUIRE(installSample(rig, 0, impulse()));
    auto p = pattern(16);
    put(*p, 0, 0);
    put(*p, 3, 0); // swung step 4 with swing 30 %
    REQUIRE(rig.param("seq.retrigCount", 4.0f, scope(0)));
    REQUIRE(rig.param("seq.retrigSpeed", 3.0f, scope(0))); // 1/3 step
    REQUIRE(rig.param("seq.swing", 30.0f));
    REQUIRE(rig.install(*p));
    REQUIRE(rig.play());
    const double step = stepFrames();
    rig.until(static_cast<long>(6 * step));
    std::vector<long> onsets;
    for (std::size_t i = 0; i < rig.mainL.size(); ++i)
        if (std::fabs(rig.mainL[i]) > 1e-6f && (i == 0 || std::fabs(rig.mainL[i - 1]) <= 1e-6f))
            onsets.push_back(static_cast<long>(i));
    const auto steps = rig.sequenced(0);
    REQUIRE(steps.size() == 2u);
    std::vector<long> want;
    for (const auto& s : steps)
        for (int k = 0; k < 4; ++k)
            want.push_back(k == 0 ? s.frame : expectedRepeat(s.frame, k, 3, 120.0));
    CHECK(onsets == want);
    CHECK(rig.e->voiceStarts() == 8);
}

TEST_CASE("perf.engine: a new step of the track cancels its pending repeats; a choke and STOP cancel them too",
          "[perf][engine]")
{
    const double step = stepFrames();
    SECTION("own step")
    {
        Rig rig;
        auto p = pattern(16);
        put(*p, 0, 2);
        put(*p, 4, 2);
        REQUIRE(rig.param("seq.retrigCount", 16.0f, scope(2)));
        REQUIRE(rig.param("seq.retrigSpeed", 5.0f, scope(2))); // 1 step: 15 repeats would run past step 5
        REQUIRE(rig.install(*p));
        REQUIRE(rig.play());
        rig.until(static_cast<long>(7.5 * step));
        const auto s = rig.sequenced(2);
        REQUIRE(s.size() == 2u);
        const auto r = repeats(rig, 2);
        REQUIRE(r.size() == 3u + 3u); // steps 2, 3, 4 of the first ratchet, then 6, 7, 8 of the second
        for (int k = 1; k <= 3; ++k)
            CHECK(r[static_cast<std::size_t>(k - 1)].frame == expectedRepeat(s[0].frame, k, 5, 120.0));
        for (int k = 1; k <= 3; ++k)
            CHECK(r[static_cast<std::size_t>(k + 2)].frame == expectedRepeat(s[1].frame, k, 5, 120.0));
        CHECK(rig.e->perf().retrig.remaining(2) == 12);
    }
    SECTION("choke group")
    {
        Rig rig;
        auto p = pattern(16);
        put(*p, 0, 0);
        put(*p, 2, 1); // track 2 of the group plays on step 3
        REQUIRE(rig.param("track.chokeGroup", 1.0f, scope(0)));
        REQUIRE(rig.param("track.chokeGroup", 1.0f, scope(1)));
        REQUIRE(rig.param("seq.retrigCount", 16.0f, scope(0)));
        REQUIRE(rig.param("seq.retrigSpeed", 4.0f, scope(0))); // every half step
        REQUIRE(rig.install(*p));
        REQUIRE(rig.play());
        rig.until(static_cast<long>(6 * step));
        const auto choke = rig.sequenced(1);
        REQUIRE(choke.size() == 1u);
        const auto r = repeats(rig, 0);
        REQUIRE(r.size() == 3u); // 0.5, 1, 1.5 steps; the one at 2 steps meets the choke at the same frame
        for (const auto& h : r)
            CHECK(h.frame < choke[0].frame);
        CHECK_FALSE(rig.e->perf().retrig.pending(0));
    }
    SECTION("STOP")
    {
        Rig rig;
        auto p = pattern(16);
        put(*p, 0, 5);
        REQUIRE(rig.param("seq.retrigCount", 16.0f, scope(5)));
        REQUIRE(rig.param("seq.retrigSpeed", 5.0f, scope(5)));
        REQUIRE(rig.install(*p));
        REQUIRE(rig.play());
        rig.until(static_cast<long>(2.5 * step));
        REQUIRE(rig.stop());
        const long stoppedAt = rig.frame;
        rig.until(static_cast<long>(20 * step));
        for (const auto& h : repeats(rig, 5))
            CHECK(h.frame < stoppedAt);
        CHECK(repeats(rig, 5).size() == 2u);
        CHECK(rig.e->perf().retrig.pendingMask() == 0);
    }
}

TEST_CASE("perf.engine: repeats sound with the step's locks; the next step reverts them (SEQ-021)", "[perf][engine]")
{
    Rig rig;
    REQUIRE(installSample(rig, 0, dc()));
    auto p = pattern(16);
    put(*p, 0, 0);
    lock(*p, 0, 0, 4, 12.0f);                                // PITCH +12 on step 1 (Revert)
    lock(*p, 0, 0, perf::kLockSlotRetrigCount, 4.0f);        // 3 repeats, one step apart
    lock(*p, 0, 0, perf::kLockSlotRetrigSpeed, 5.0f);
    put(*p, 4, 0);                                           // step 5, no lock
    REQUIRE(rig.install(*p));
    REQUIRE(rig.play());
    const double st = stepFrames();
    for (int s = 0; s < 4; ++s) // the step and its 3 repeats: +12 st
        CHECK(rate(rig, 0, static_cast<long>((s + 0.2) * st), static_cast<long>((s + 0.8) * st)) ==
              Catch::Approx(2.0).margin(0.01));
    CHECK(rate(rig, 0, static_cast<long>(4.2 * st), static_cast<long>(4.8 * st)) == Catch::Approx(1.0).margin(0.01));
    CHECK(rig.e->voiceStarts() == 5);
}

TEST_CASE("perf.engine: ratchets restart the envelope with RETRIGGER on (golden: a single trigger's shape) and leave "
          "a running one alone with it off (ENV-007)",
          "[perf][engine][envelope]")
{
    const double st = stepFrames();
    // Attack 0, HOLD ≈ 307 ms, RELEASE ≈ 50 ms; a DC sample, so the output is the envelope.
    auto render = [&](int count, bool retrigger, std::vector<float>& out, long& t0) {
        Rig rig;
        REQUIRE(installSample(rig, 0, dc()));
        REQUIRE(rig.param("env.attack", 0.0f, scope(0)));
        REQUIRE(rig.param("env.hold", 79.0f, scope(0)));
        REQUIRE(rig.param("env.release", 50.0f, scope(0)));
        REQUIRE(rig.param("env.retrigger", retrigger ? 1.0f : 0.0f, scope(0)));
        REQUIRE(rig.param("seq.retrigCount", static_cast<float>(count), scope(0)));
        REQUIRE(rig.param("seq.retrigSpeed", 4.0f, scope(0))); // every half step (62.5 ms)
        auto p = pattern(64);
        put(*p, 0, 0);
        REQUIRE(rig.install(*p));
        REQUIRE(rig.play());
        rig.until(static_cast<long>(12 * st));
        t0 = rig.sequenced(0).at(0).frame;
        out = rig.mainL;
        return repeats(rig, 0);
    };
    std::vector<float> ref, on, off;
    long t0 = 0, t1 = 0, t2 = 0;
    REQUIRE(render(1, true, ref, t0).empty());
    const auto rOn = render(4, true, on, t1);
    const auto rOff = render(4, false, off, t2);
    REQUIRE(rOn.size() == 3u);
    REQUIRE(rOff.size() == 3u);
    REQUIRE(t0 == t1);
    const long last = rOn.back().frame; // the last repeat, 187.5 ms after the step
    const long fade = 100;              // past the old note's 2 ms hand-off tail (PLY-042)
    const long len = static_cast<long>(0.36 * 44100.0);
    float peakRef = 0.0f;
    for (long i = fade; i < len; ++i)
    {
        // RETRIGGER on: after the last repeat the envelope replays the single trigger's shape from its start.
        REQUIRE(on.at(static_cast<std::size_t>(last + i)) == Catch::Approx(ref.at(static_cast<std::size_t>(t0 + i))).margin(1e-6));
        peakRef = std::max(peakRef, ref[static_cast<std::size_t>(t0 + i)]);
    }
    // RETRIGGER off: the envelope started at the step runs on (the release begins ≈ 307 ms after the step).
    for (long i = last - t0 + fade; i < len; ++i)
        REQUIRE(off.at(static_cast<std::size_t>(t0 + i)) == Catch::Approx(ref.at(static_cast<std::size_t>(t0 + i))).margin(1e-6));
    const auto at = static_cast<std::size_t>(t0 + static_cast<long>(0.33 * 44100.0)); // in the reference's release
    CHECK(off[at] < 0.8f * peakRef);
    CHECK(on[at] == Catch::Approx(peakRef).margin(1e-6)); // still holding
}

TEST_CASE("perf.engine: transpose moves the voice by exactly its semitones, per scene and track", "[perf][engine][transpose]")
{
    const double st = stepFrames();
    for (const int semis : {-24, -7, 0, 1, 7, 12, 24})
    {
        INFO("transpose " << semis);
        Rig rig;
        REQUIRE(installSample(rig, 3, dc()));
        auto p = pattern(16);
        put(*p, 0, 3);
        REQUIRE(rig.param("seq.transpose", static_cast<float>(semis), scope(3)));
        REQUIRE(rig.param("seq.transpose", 5.0f, scope(3, 1))); // another scene: not this one's
        REQUIRE(rig.install(*p));
        REQUIRE(rig.play());
        const double r = rate(rig, 3, static_cast<long>(0.1 * st), static_cast<long>(3.9 * st));
        CHECK(12.0 * std::log2(r) == Catch::Approx(static_cast<double>(semis)).margin(0.005));
        CHECK(rig.e->perf().transpose(3) == semis);
        CHECK(rig.e->perf().transpose(1, 3) == 5);
    }
}

TEST_CASE("perf.engine: transpose changes the sounding note at the next block and clamps to +-24", "[perf][engine][transpose]")
{
    Rig rig;
    REQUIRE(installSample(rig, 0, dc()));
    auto p = pattern(16);
    put(*p, 0, 0);
    REQUIRE(rig.install(*p));
    REQUIRE(rig.play());
    const double st = stepFrames();
    CHECK(rate(rig, 0, static_cast<long>(0.1 * st), static_cast<long>(0.4 * st)) == Catch::Approx(1.0).margin(1e-3));
    REQUIRE(rig.param("seq.transpose", 99.0f, scope(0))); // clamped to +24
    rig.block();
    CHECK(rate(rig, 0, static_cast<long>(0.5 * st), static_cast<long>(0.9 * st)) == Catch::Approx(4.0).margin(1e-3));
    REQUIRE(rig.param("seq.transpose", 0.0f, scope(0)));
    rig.block();
    CHECK(rate(rig, 0, static_cast<long>(1.0 * st), static_cast<long>(1.4 * st)) == Catch::Approx(1.0).margin(1e-3));
}

TEST_CASE("perf.engine: a scene switch ends the old scene's ratchets at the switch frame (our choice)",
          "[perf][engine]")
{
    Rig rig;
    auto p = pattern(16);
    put(*p, 12, 0); // step 13: 15 repeats one step apart would run far into the next pass
    REQUIRE(rig.param("seq.retrigCount", 16.0f, scope(0)));
    REQUIRE(rig.param("seq.retrigSpeed", 5.0f, scope(0)));
    REQUIRE(rig.install(*p));
    REQUIRE(rig.play());
    rig.blocks(4);
    REQUIRE(rig.queue(1, 0)); // playing: scene 2 at the next boundary (the end of the pass)
    rig.until(static_cast<long>(20 * stepFrames()));
    bool switched = false;
    for (const auto& ev : rig.events())
        switched = switched || (ev.kind == engine::EngineEvent::Kind::SwitchDone && ev.value == 16);
    REQUIRE(switched);
    const auto s = rig.sequenced(0);
    REQUIRE(s.size() == 1u); // scene 2 is empty
    const auto r = repeats(rig, 0);
    REQUIRE(r.size() == 3u); // steps 14, 15, 16; the 4th falls on the switch (16 steps = the next pass's start)
    for (int k = 1; k <= 3; ++k)
        CHECK(r[static_cast<std::size_t>(k - 1)].frame == expectedRepeat(s[0].frame, k, 5, 120.0));
    CHECK_FALSE(rig.e->perf().retrig.pending(0));
}

TEST_CASE("perf.engine: through the ModMatrix, stored pitch + transpose clamps to +-24 st; transpose is the matrix's",
          "[perf][engine][transpose]")
{
    const double st = stepFrames();
    struct Case
    {
        float pitch;
        int transpose;
        double want; // semitones sounding
    };
    for (const Case c : {Case {12.0f, 7, 19.0}, Case {12.0f, 24, 24.0}, Case {-12.0f, -24, -24.0}, Case {-5.0f, 5, 0.0}})
    {
        INFO("pitch " << c.pitch << " transpose " << c.transpose);
        Rig rig;
        REQUIRE(installSample(rig, 0, dc()));
        auto p = pattern(16);
        put(*p, 0, 0);
        REQUIRE(rig.param("playback.pitch", c.pitch, scope(0)));
        REQUIRE(rig.param("seq.transpose", static_cast<float>(c.transpose), scope(0)));
        REQUIRE(rig.install(*p));
        REQUIRE(rig.play());
        const double r = rate(rig, 0, static_cast<long>(0.1 * st), static_cast<long>(3.9 * st));
        CHECK(12.0 * std::log2(r) == Catch::Approx(c.want).margin(0.005));
        const auto pitch = rig.e->perf().pitchParam();
        CHECK(rig.e->modMatrix().contribution(0, pitch, engine::modulation::ModSource::Transpose) ==
              engine::modulation::pitchOffset01(static_cast<float>(c.transpose) * 100.0f));
    }
}
