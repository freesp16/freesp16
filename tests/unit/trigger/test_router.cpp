// TriggerRouter + TrackSource (TRK-010/011/020, SEQ-011/015, PAD-001/004): frame order, choke groups with the 2 ms
// fade, GATE release at the event frame, sequencer events ignoring GATE, pad routing by pad mode, MIDI notes, Thru
// pass-through without an envelope, live-hit sink, ducks flags.
#include "TriggerRig.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace trigtest;
using engine::TriggerNow;

namespace
{
constexpr int kFade = 88; // 2 ms at 44.1 kHz (TrackVoice::fadeFrames)

float at(const Rig& r, int track, int block, int frame)
{
    return r.history[static_cast<std::size_t>(track)][static_cast<std::size_t>(block * kBlockFrames + frame)];
}

struct Sink final : LiveHitSink
{
    int hits = 0;
    TriggerEvent last {};
    bool pitched = false;
    void onLiveHit(const TriggerEvent& e, bool p) noexcept override
    {
        ++hits;
        last = e;
        pitched = p;
    }
};
} // namespace

TEST_CASE("trigger.router: pad 5 in PLAY mode starts track 5 at the block's frame 0", "[trigger]")
{
    TestBuffer buf(dc(20000));
    auto r = makeRig();
    r->src[4].setSample(buf.get());
    REQUIRE(r->router.pushLive(TriggerNow {4, 100, 60, 4}));
    const auto active = r->block();
    CHECK(active == 1u << 4);
    CHECK(at(*r, 4, 0, 0) > 0.0f); // sounding from frame 0
    REQUIRE(r->router.blockEvents().size() == 1);
    const auto& e = r->router.blockEvents()[0];
    CHECK(e.track == 4);
    CHECK(e.frame == 0);
    CHECK(e.kind == TriggerKind::Full);
    CHECK(e.source == TriggerSource::Pad);
    CHECK(e.ducks);
    CHECK((r->ctx.triggerMask & (1u << 4)) != 0);
    CHECK(r->src[4].voice().mainActive());
}

TEST_CASE("trigger.router: events are applied in frame order (stable), whatever order they arrive in", "[trigger]")
{
    auto r = makeRig();
    REQUIRE(r->router.push(Rig::event(3, TriggerKind::Full, 40)));
    REQUIRE(r->router.push(Rig::event(1, TriggerKind::Full, 5)));
    REQUIRE(r->router.push(Rig::event(2, TriggerKind::Full, 40)));
    REQUIRE(r->router.push(Rig::event(0, TriggerKind::Full, 0)));
    r->block();
    const auto ev = r->router.blockEvents();
    REQUIRE(ev.size() == 4);
    CHECK(ev[0].track == 0);
    CHECK(ev[1].track == 1);
    CHECK(ev[2].track == 3); // same frame: arrival order kept
    CHECK(ev[3].track == 2);
    CHECK(r->ctx.triggerOffset[3] == 40);
    // 64 per block, then refused (and counted).
    for (int i = 0; i < TriggerRouter::kMaxEvents; ++i)
        REQUIRE(r->router.push(Rig::event(i & 15, TriggerKind::Full, 63 - i)));
    CHECK_FALSE(r->router.push(Rig::event(0, TriggerKind::Full, 0)));
    CHECK(r->router.dropped() == 1);
    r->block();
    for (std::size_t i = 1; i < r->router.blockEvents().size(); ++i)
        CHECK(r->router.blockEvents()[i - 1].frame <= r->router.blockEvents()[i].frame);
}

TEST_CASE("trigger.router: choke group 1 on tracks 1-3 leaves exactly one sounding, with a 2 ms fade", "[trigger]")
{
    TestBuffer buf(dc(100000));
    auto r = makeRig();
    for (int t = 0; t < 3; ++t)
    {
        r->src[static_cast<std::size_t>(t)].setSample(buf.get());
        r->param(t, TrackField::ChokeGroup, 1.0f);
    }
    CHECK(r->router.chokeMask(0, 1) == 0b111);
    r->router.push(Rig::event(0, TriggerKind::Full, 0));
    r->blocks(2);
    const float level = at(*r, 0, 1, 10);
    REQUIRE(level > 0.0f);
    r->router.push(Rig::event(1, TriggerKind::Full, 10)); // block 2, frame 10
    r->blocks(3);
    CHECK(at(*r, 0, 2, 9) == Catch::Approx(level));          // untouched before the choke frame
    CHECK(at(*r, 0, 2, 10 + kFade / 2) < 0.75f * level);       // fading …
    CHECK(at(*r, 0, 2, 10 + kFade / 2) > 0.25f * level);       // … linearly, not cut
    CHECK(at(*r, 0, 3, 10 + kFade - kBlockFrames) == 0.0f);    // silent 2 ms after the choke
    CHECK(at(*r, 1, 2, 10) > 0.0f);                            // the new hit sounds from its frame
    CHECK(at(*r, 1, 2, 9) == 0.0f);
    r->router.push(Rig::event(2, TriggerKind::Full, 0));
    r->blocks(4);
    int sounding = 0;
    for (int t = 0; t < 3; ++t)
        sounding += r->src[static_cast<std::size_t>(t)].voice().active() ? 1 : 0;
    CHECK(sounding == 1);
    CHECK(r->src[2].voice().mainActive());
    // A track outside the group keeps playing; group "--" chokes nothing.
    r->src[5].setSample(buf.get());
    r->router.push(Rig::event(5, TriggerKind::Full, 0));
    r->blocks(1);
    r->router.push(Rig::event(1, TriggerKind::Full, 0));
    r->blocks(4);
    CHECK(r->src[5].voice().mainActive());
    CHECK(r->src[1].voice().mainActive());
    CHECK_FALSE(r->src[2].voice().active());
    // Leaving the group removes the track from the mask.
    r->param(2, TrackField::ChokeGroup, 0.0f);
    CHECK(r->router.chokeMask(0, 1) == 0b011);
}

TEST_CASE("trigger.router: GATE release enters the envelope's release at the event frame", "[trigger]")
{
    TestBuffer buf(dc(100000));
    auto r = makeRig();
    r->src[0].setSample(buf.get());
    engine::modulation::AmpEnvParams p;
    p.release = 80; // ≈ 0.1 s
    r->env(0, p);
    r->param(0, TrackField::TriggerMode, 1.0f); // GATE
    REQUIRE(r->router.pushLive(TriggerNow {0, 127, 60, 0}));
    r->blocks(3);
    const float held = at(*r, 0, 2, 63);
    REQUIRE(held > 0.0f);
    // Pad up arrives at frame 0 of the next block (live); a frame-accurate release (e.g. MIDI) at frame 20.
    r->router.push(Rig::event(0, TriggerKind::Release, 20, TriggerSource::Midi, 0));
    r->block();
    CHECK(at(*r, 0, 3, 19) == Catch::Approx(held));
    CHECK(at(*r, 0, 3, 20) <= held);
    CHECK(at(*r, 0, 3, 40) < held);
    CHECK(r->bank.stage(0) == engine::modulation::AmpEnvStage::Release);
}

TEST_CASE("trigger.router: a sequencer-sourced event ignores GATE (SEQ-015)", "[trigger]")
{
    TestBuffer buf(dc(100000));
    auto r = makeRig();
    r->src[0].setSample(buf.get());
    engine::modulation::AmpEnvParams p;
    p.release = 80;
    r->env(0, p);
    r->param(0, TrackField::TriggerMode, 1.0f); // GATE
    r->router.push(Rig::event(0, TriggerKind::Full, 0, TriggerSource::Sequencer));
    r->blocks(2);
    const float held = at(*r, 0, 1, 63);
    r->router.push(Rig::event(0, TriggerKind::Release, 20, TriggerSource::Pad, 0));
    r->block();
    CHECK(at(*r, 0, 2, 40) == Catch::Approx(held)); // One Shot: the hold (INFINITY) continues
    CHECK(r->bank.stage(0) == engine::modulation::AmpEnvStage::Hold);
    // The same track's pad hit is gated.
    r->router.pushLive(TriggerNow {0, 127, 60, 0});
    r->blocks(2);
    r->router.pushLive(TriggerNow {0, 0, 60, 0});
    r->block();
    CHECK(r->bank.stage(0) == engine::modulation::AmpEnvStage::Release);
}

TEST_CASE("trigger.router: HALF restarts the envelope only on a sounding voice and never restarts the sample", "[trigger]")
{
    TestBuffer buf(voicetest::ramp(100000, 0.0f, 1e-5f));
    auto r = makeRig();
    r->src[0].setSample(buf.get());
    r->router.push(Rig::event(1, TriggerKind::Half, 0)); // idle track 2: nothing starts (SEQ-011)
    r->block();
    CHECK((r->active & 2u) == 0);
    r->router.push(Rig::event(0, TriggerKind::Full, 0));
    r->blocks(4);
    const auto before = r->src[0].voice().position();
    r->router.push(Rig::event(0, TriggerKind::Half, 0));
    r->block();
    CHECK(r->src[0].voice().position() == before + kBlockFrames); // the sample continues
}

TEST_CASE("trigger.router: pad modes route pad hits (PAD-001/004), releases follow their press", "[trigger]")
{
    auto r = makeRig();
    Sink sink;
    r->router.setLiveHitSink(&sink);
    r->router.setPadParam(PadField::Selected, 6.0f);

    r->router.setPadParam(PadField::Mode, static_cast<float>(PadMode::Track));
    r->router.pushLive(TriggerNow {2, 100, 60, 2});
    r->block();
    CHECK(r->router.blockEvents().empty()); // TRACK: select only
    r->router.setPadParam(PadField::Mode, static_cast<float>(PadMode::Mute));
    r->router.pushLive(TriggerNow {2, 100, 60, 2});
    r->block();
    CHECK(r->router.blockEvents().empty()); // MUTE: no sound

    r->router.setPadParam(PadField::Mode, static_cast<float>(PadMode::Scale));
    r->param(6, TrackField::ScaleType, 1.0f);      // IONIAN (major) on the selected track
    r->router.pushLive(TriggerNow {7, 90, 60, 7}); // pad 8 = degree 7 = the octave
    r->block();
    REQUIRE(r->router.blockEvents().size() == 1);
    CHECK(r->router.blockEvents()[0].track == 6);
    CHECK(r->router.blockEvents()[0].semitones == 12);
    CHECK(sink.hits == 1);
    CHECK(sink.last.track == 6);
    CHECK(sink.pitched); // a SCALE pad: the recorder keeps its semitones

    // The mode changes while pad 8 is held: its release still goes to track 7 (index 6), with its pitch.
    r->router.setPadParam(PadField::Mode, static_cast<float>(PadMode::Play));
    r->router.pushLive(TriggerNow {7, 0, 60, 7});
    r->block();
    REQUIRE(r->router.blockEvents().size() == 1);
    CHECK(r->router.blockEvents()[0].kind == TriggerKind::Release);
    CHECK(r->router.blockEvents()[0].track == 6);
    CHECK(sink.hits == 1); // releases are not live hits
    r->router.pushLive(TriggerNow {7, 0, 60, 7}); // a second release (no press held): nothing
    r->block();
    CHECK(r->router.blockEvents().empty());

    r->router.setPadParam(PadField::Mode, static_cast<float>(PadMode::Slice));
    r->router.pushLive(TriggerNow {3, 90, 60, 3});
    r->block();
    REQUIRE(r->router.blockEvents().size() == 1);
    CHECK(r->router.blockEvents()[0].track == 6); // SLICE plays the selected track (the slice lock picks the slice)

    r->router.setPadParam(PadField::Mode, static_cast<float>(PadMode::Play));
    r->router.setPadParam(PadField::Shift, 1.0f);
    r->router.pushLive(TriggerNow {3, 90, 60, 3});
    r->block();
    CHECK(r->router.blockEvents().empty()); // SHIFT + pad selects, no sound
}

TEST_CASE("trigger.router: SCALE pads of a MIDI track play SET SCALE (MIDI) from its root over NOTE 1 (PAD-020)",
          "[trigger]")
{
    auto r = makeRig();
    Sink sink;
    r->router.setLiveHitSink(&sink);
    r->router.setPadParam(PadField::Selected, 3.0f);
    r->router.setPadParam(PadField::Mode, static_cast<float>(PadMode::Scale));
    r->param(3, TrackField::ScaleType, 1.0f);       // SET SCALE (Sample) IONIAN: not used by a MIDI track
    r->param(3, TrackField::Mode, 1.0f);            // MIDI
    r->param(3, TrackField::MidiScaleType, 6.0f);   // AEOLIAN (C D Eb F G Ab Bb)
    r->param(3, TrackField::MidiScaleRoot, 62.0f);  // D4
    r->router.setMidiNote1(0, 3, 60);               // NOTE 1 = C4
    r->router.pushLive(TriggerNow {2, 100, 60, 2}); // pad 3 = degree 2 = a minor third over the root: F4 (65)
    r->block();
    REQUIRE(r->router.blockEvents().size() == 1);
    CHECK(r->router.blockEvents()[0].track == 3);
    CHECK(r->router.blockEvents()[0].semitones == 65 - 60); // NOTE 1 (C4) + 5 = F4; harmony notes follow
    CHECK(sink.pitched);

    r->param(3, TrackField::MidiScaleArea, 1.0f);   // window 1: pad 1 = degree 16 = two octaves + a 3rd
    r->router.pushLive(TriggerNow {0, 100, 60, 0});
    r->block();
    REQUIRE(r->router.blockEvents().size() == 1);
    CHECK(r->router.blockEvents()[0].semitones == 62 + 24 + 3 - 60);

    r->param(3, TrackField::Mode, 0.0f);            // back to SAMPLE: SET SCALE (Sample) again
    r->router.pushLive(TriggerNow {7, 100, 60, 7}); // MAJOR, pad 8 = the octave
    r->block();
    REQUIRE(r->router.blockEvents().size() == 1);
    CHECK(r->router.blockEvents()[0].semitones == 12);
}

TEST_CASE("trigger.router: SLICE pads are silent on a MIDI or AS-1 track (the manual: no SLICE mode there)",
          "[trigger]")
{
    auto r = makeRig();
    r->router.setPadParam(PadField::Selected, 5.0f);
    r->router.setPadParam(PadField::Mode, static_cast<float>(PadMode::Slice));
    for (const float mode : {1.0f, 3.0f})
    {
        r->param(5, TrackField::Mode, mode);
        r->router.pushLive(TriggerNow {1, 100, 60, 1});
        r->block();
        CHECK(r->router.blockEvents().empty());
    }
    r->param(5, TrackField::Mode, 0.0f); // a Sample track plays its slices
    r->router.pushLive(TriggerNow {1, 100, 60, 1});
    r->block();
    CHECK(r->router.blockEvents().size() == 1);
}

TEST_CASE("trigger.router: MIDI notes (pad -1) play their track; note offset and note off", "[trigger]")
{
    auto r = makeRig();
    Sink sink;
    r->router.setLiveHitSink(&sink);
    r->router.pushLive(TriggerNow {9, 80, 67, -1});
    r->block();
    REQUIRE(r->router.blockEvents().size() == 1);
    const auto e = r->router.blockEvents()[0];
    CHECK(e.track == 9);
    CHECK(e.source == TriggerSource::Midi);
    CHECK(e.semitones == 7);
    CHECK(e.velocity == 80);
    CHECK(sink.hits == 1);
    CHECK(sink.pitched); // a MIDI note: the recorder keeps its semitones
    r->router.pushLive(TriggerNow {9, 0, 67, -1});
    r->block();
    REQUIRE(r->router.blockEvents().size() == 1);
    CHECK(r->router.blockEvents()[0].kind == TriggerKind::Release);
}

TEST_CASE("trigger.router: muted tracks do not duck; mixer locks are applied per trigger", "[trigger]")
{
    auto r = makeRig();
    engine::mixer::StripParams sp;
    sp.mute = true;
    r->mixer.setStrip(0, 3, sp, 0);
    r->router.push(Rig::event(3, TriggerKind::Full, 0));
    r->router.push(Rig::event(4, TriggerKind::Full, 0));
    r->router.push(Rig::event(4, TriggerKind::LockOnly, 1));
    r->block();
    const auto ev = r->router.blockEvents();
    REQUIRE(ev.size() == 3);
    CHECK_FALSE(ev[0].ducks);
    CHECK(ev[1].ducks);
    CHECK_FALSE(ev[2].ducks);
}

TEST_CASE("trigger.source: Thru passes the input through bit-exactly with no envelope; mode changes fade", "[trigger]")
{
    auto r = makeRig();
    alignas(16) float inL[kBlockFrames], inR[kBlockFrames];
    for (int i = 0; i < kBlockFrames; ++i)
    {
        inL[i] = 0.3f * std::sin(0.1f * static_cast<float>(i));
        inR[i] = -0.2f;
    }
    r->param(2, TrackField::Mode, 2.0f); // THRU
    r->block(inL, inR); // fade in (2 ms)
    r->block(inL, inR);
    r->block(inL, inR);
    REQUIRE((r->active & 4u) != 0);
    for (int i = 0; i < kBlockFrames; ++i)
    {
        REQUIRE(r->out[2].l[i] == inL[i]);
        REQUIRE(r->out[2].r[i] == inR[i]);
    }
    CHECK(r->bank.stage(2) == engine::modulation::AmpEnvStage::Thru);
    // Triggers do not gate a Thru track.
    r->router.push(Rig::event(2, TriggerKind::Full, 0));
    r->block(inL, inR);
    CHECK(r->out[2].l[10] == inL[10]);
    // INPUT SOURCE L: the left channel on both sides.
    r->param(2, TrackField::InputSource, 1.0f);
    r->block(inL, inR);
    CHECK(r->out[2].r[5] == inL[5]);
    // Back to SAMPLE: the input fades out over 2 ms, then the track is silent.
    r->param(2, TrackField::Mode, 0.0f);
    r->block(inL, inR);
    CHECK(std::fabs(r->out[2].l[63]) < std::fabs(inL[63]) + 1e-6f);
    r->block(inL, inR);
    r->block(inL, inR);
    CHECK((r->active & 4u) == 0);
}

TEST_CASE("trigger.source: changing the mode stops a sounding sample with a 2 ms fade (TRK-002)", "[trigger]")
{
    TestBuffer buf(dc(100000));
    auto r = makeRig();
    r->src[0].setSample(buf.get());
    r->router.push(Rig::event(0, TriggerKind::Full, 0));
    r->blocks(2);
    const float level = at(*r, 0, 1, 63);
    r->param(0, TrackField::Mode, 1.0f); // MIDI
    r->blocks(2);
    CHECK(at(*r, 0, 2, 0) == Catch::Approx(level).epsilon(0.05));
    CHECK(at(*r, 0, 2, kFade / 2) < level);
    CHECK(at(*r, 0, 3, kFade - kBlockFrames) == 0.0f);
    CHECK_FALSE(r->src[0].voice().active());
    r->router.push(Rig::event(0, TriggerKind::Full, 0)); // MIDI track: no audio source
    r->block();
    CHECK((r->active & 1u) == 0);
}

TEST_CASE("trigger.source: the voice ends with a non-looping sample, its envelope is reset", "[trigger]")
{
    TestBuffer buf(dc(100));
    auto r = makeRig();
    r->src[0].setSample(buf.get());
    r->router.push(Rig::event(0, TriggerKind::Full, 0));
    r->blocks(4);
    CHECK_FALSE(r->src[0].voice().active());
    CHECK(r->bank.finished(0));
    CHECK((r->active & 1u) == 0);
}

TEST_CASE("trigger.router: MUTE MODE STOP STEPS skips a muted track's steps; pads still sound", "[trigger]")
{
    TestBuffer buf(dc(100000));
    auto r = makeRig();
    r->src[0].setSample(buf.get());
    engine::mixer::StripParams muted;
    muted.mute = true;
    r->mixer.setStrip(0, 0, muted, 0);
    // STOP SOUND (default): the step starts the voice (the mixer's gate silences the strip).
    CHECK_FALSE(r->mixer.stepsMuted(0));
    r->router.push(Rig::event(0, TriggerKind::Full, 0, TriggerSource::Sequencer));
    CHECK((r->block() & 1u) != 0);
    r->src[0].choke(0);
    r->blocks(4);
    // STOP STEPS (this track's own mode): the muted track's sequencer step does nothing; a pad hit on it plays, and
    // its strip stays open.
    muted.stopSteps = true;
    r->mixer.setStrip(0, 0, muted, 0);
    CHECK(r->mixer.stepsMuted(0));
    CHECK_FALSE(r->mixer.stepsMuted(1));
    CHECK((r->mixer.audibleMask() & 1u) != 0);
    r->router.push(Rig::event(0, TriggerKind::Full, 0, TriggerSource::Sequencer));
    CHECK((r->block() & 1u) == 0);
    r->router.pushLive(TriggerNow {0, 127, 60, 0});
    CHECK((r->block() & 1u) != 0);
    // Unmuted: the steps play again.
    engine::mixer::StripParams open;
    open.stopSteps = true;
    r->mixer.setStrip(0, 0, open, 0);
    CHECK_FALSE(r->mixer.stepsMuted(0));
}

// The pad-flash feed (PlayheadFeed::trackHits): every Full / Half trigger a track takes counts, from any source; a
// release, a lock-only event and a muted track's skipped step (STOP STEPS) do not.
TEST_CASE("trigger.router: hits(t) counts each track's applied hits for the pad flashes", "[trigger]")
{
    TestBuffer buf(dc(100000));
    auto r = makeRig();
    r->src[0].setSample(buf.get());
    r->src[1].setSample(buf.get());
    CHECK(r->router.hits(0) == 0);
    r->router.push(Rig::event(0, TriggerKind::Full, 0, TriggerSource::Sequencer));
    r->router.push(Rig::event(0, TriggerKind::Half, 32, TriggerSource::Sequencer));
    r->router.push(Rig::event(1, TriggerKind::LockOnly, 10, TriggerSource::Sequencer));
    r->block();
    CHECK(r->router.hits(0) == 2);
    CHECK(r->router.hits(1) == 0);
    REQUIRE(r->router.pushLive(TriggerNow {1, 100, 60, 1})); // a pad
    r->block();
    CHECK(r->router.hits(1) == 1);
    REQUIRE(r->router.pushLive(TriggerNow {1, 0, 60, 1})); // its release
    r->block();
    CHECK(r->router.hits(1) == 1);
    engine::mixer::StripParams muted;
    muted.mute = true;
    muted.stopSteps = true; // STOP STEPS: the step is skipped
    r->mixer.setStrip(0, 0, muted, 0);
    r->router.push(Rig::event(0, TriggerKind::Full, 0, TriggerSource::Sequencer));
    r->block();
    CHECK(r->router.hits(0) == 2);
}

TEST_CASE("mixer: MUTE MODE is per track — STOP SOUND and STOP STEPS tracks side by side, under mute and solo",
          "[mixer]")
{
    engine::mixer::Mixer m;
    engine::mixer::StripParams sound, steps;
    steps.stopSteps = true;
    // Track 1 STOP SOUND muted: gated, steps run. Track 2 STOP STEPS muted: open, steps skipped.
    sound.mute = steps.mute = true;
    m.setStrip(0, 0, sound, 0);
    m.setStrip(0, 1, steps, 0);
    CHECK((m.audibleMask() & 0b11u) == 0b10u);
    CHECK_FALSE(m.stepsMuted(0));
    CHECK(m.stepsMuted(1));
    // Unmuted, track 3 soloed: track 1 (STOP SOUND) gated, track 2 (STOP STEPS) sounds with its steps skipped.
    sound.mute = steps.mute = false;
    m.setStrip(0, 0, sound, 0);
    m.setStrip(0, 1, steps, 0);
    engine::mixer::StripParams solo;
    solo.solo = true;
    m.setStrip(0, 2, solo, 0);
    CHECK((m.audibleMask() & 0b111u) == 0b110u);
    CHECK_FALSE(m.stepsMuted(0));
    CHECK(m.stepsMuted(1));
    CHECK_FALSE(m.stepsMuted(2));
}
