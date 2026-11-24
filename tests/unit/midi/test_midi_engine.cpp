// MIDI in the engine: incoming notes at arrival + one block exactly (MID-020/021/022), the CC
// map incl. FX page positions (MID-030/031/035, FX-086), CC 0/32 (SCN-050/051), CC 123 (MID-023), MIDI tracks
// (MID-040…050), the hanging-note guard (MID-047), the clock follower's phase lock (SYN-012: ≤ 1 sample per bar).
// Every render runs through the engine's real-time contract (any allocation on the audio thread aborts).
#include "tests/unit/midi/MidiTestUtil.h"

#include "engine/fx/EffectRegistryOrder.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace
{
using engine::trigger::TriggerKind;
constexpr double kRate = 44100.0;

void push(engine::Engine& e, std::uint8_t b0, std::uint8_t b1, std::uint8_t b2, double frame, int port = 0)
{
    REQUIRE(e.midi().hub().pushIn(port, miditest::msg(b0, b1, b2, miditest::hostNs(frame), port)));
}

int insertTypeIndex(engine::Engine& e, std::string_view id)
{
    const auto types = e.fxBank().insertTypes();
    for (std::size_t i = 0; i < types.size(); ++i)
        if (types[i]->id == id)
            return static_cast<int>(i);
    return -1;
}

std::vector<core::midi::UiEvent> drainUi(engine::Engine& e)
{
    std::vector<core::midi::UiEvent> v;
    core::midi::UiEvent u;
    while (e.midi().hub().popUi(u))
        v.push_back(u);
    return v;
}
} // namespace

TEST_CASE("midi.engine: incoming notes 0-15 land at arrival + one block exactly (MID-020)", "[midi]")
{
    auto e = miditest::make();
    miditest::Driver d {*e, 64};
    d.blocks(10);
    REQUIRE(e->midi().inputLatency() == 64);
    std::vector<std::int64_t> arrivals;
    for (int i = 0; i < 40; ++i)
    {
        const double a = static_cast<double>(d.frame()) - 64 + (i * 23) % 64; // arrived during the previous period
        arrivals.push_back(static_cast<std::int64_t>(a));
        push(*e, static_cast<std::uint8_t>(0x90 | (i % 16)), static_cast<std::uint8_t>(i % 16), static_cast<std::uint8_t>(1 + i), a);
        d.blocks(1);
        push(*e, 0x80, static_cast<std::uint8_t>(i % 16), 0, static_cast<double>(d.frame()) - 10); // Note Off
        d.blocks(3);
    }
    d.blocks(4);
    std::vector<miditest::MidiHit> on;
    for (const auto& h : d.hits)
        if (h.kind == TriggerKind::Full)
            on.push_back(h);
    REQUIRE(on.size() == arrivals.size());
    for (std::size_t i = 0; i < on.size(); ++i)
    {
        CHECK(on[i].frame == arrivals[i] + 64);
        CHECK(on[i].track == static_cast<int>(i % 16));
        CHECK(on[i].velocity == static_cast<int>(1 + i));
        CHECK(on[i].semitones == 0);
    }
    const auto releases = std::count_if(d.hits.begin(), d.hits.end(), [](const auto& h) { return h.kind == TriggerKind::Release; });
    CHECK(releases == static_cast<long>(arrivals.size()));
    CHECK(e->midi().stats().late == 0);

    // MID-022: a MIDI-mode track ignores incoming notes; IN NOTE/CC off ignores them all (MID-012).
    d.hits.clear();
    d.param("track.mode", 3, 1.0f); // scene 0, track 4 → MIDI
    push(*e, 0x90, 3, 100, static_cast<double>(d.frame()) - 5);
    push(*e, 0x90, 4, 100, static_cast<double>(d.frame()) - 5);
    d.blocks(3);
    REQUIRE(d.hits.size() == 1);
    CHECK(d.hits[0].track == 4);
    d.hits.clear();
    d.param("midi.in.noteCc", 0, 0.0f);
    push(*e, 0x90, 4, 100, static_cast<double>(d.frame()) - 5);
    d.blocks(3);
    CHECK(d.hits.empty());
}

TEST_CASE("midi.engine: notes 24-72 play the receive channel's tracks chromatically (MID-021)", "[midi]")
{
    auto e = miditest::make();
    miditest::Driver d {*e, 64};
    d.param("midi.in.channel", 5, 1.0f); // track 6 shares channel 1 with track 1 (1.60 rule)
    d.param("playback.timeStretch", 5, 2.0f); // track 6: MASTER TEMPO → ±12
    for (int note : {24, 47, 48, 60, 72, 23, 73})
        push(*e, 0x90, static_cast<std::uint8_t>(note), 90, static_cast<double>(d.frame()) - 1);
    d.blocks(3);
    std::vector<std::pair<int, int>> got;
    for (const auto& h : d.hits)
        got.emplace_back(h.track, h.semitones);
    const std::vector<std::pair<int, int>> want {{0, -24}, {5, -12}, {0, -1}, {5, -1}, {0, 0}, {5, 0}, {0, 12},
                                                 {5, 12},  {0, 24}, {5, 12}};
    CHECK(got == want);
}

TEST_CASE("midi.engine: a note recorded while REC is armed keeps its semitones; a track note is pitched at 0",
          "[midi][record]")
{
    auto e = miditest::make();
    miditest::Driver d {*e, 64};
    d.param("seq.record", 0, 1.0f);
    e->bridge().post(engine::TransportCmd {engine::TransportCmd::Play, 0.0});
    d.blocks(4);
    engine::EngineEvent ev;
    while (e->bridge().popEvent(ev)) {}
    std::vector<engine::EngineEvent> got;
    auto drain = [&] {
        while (e->bridge().popEvent(ev))
            if (ev.kind == engine::EngineEvent::Kind::RecordedHit)
                got.push_back(ev);
    };
    push(*e, 0x90, 55, 100, static_cast<double>(d.frame()) - 1); // G3 on channel 1: track 1, +7 st (MID-021)
    d.blocks(3);
    drain();
    REQUIRE(got.size() == 1u);
    CHECK(got[0].track == 0);
    CHECK(got[0].value == 100);
    CHECK(got[0].pitched);
    CHECK(got[0].semitones == 55 - 48);
    push(*e, 0x90, 3, 90, static_cast<double>(d.frame()) - 1); // note 3: track 4 at its own pitch (MID-020)
    d.blocks(3);
    drain();
    REQUIRE(got.size() == 2u);
    CHECK(got[1].track == 3);
    CHECK(got[1].pitched);
    CHECK(got[1].semitones == 0);
}

TEST_CASE("midi.engine: CC 46 at 0/64/127 hits the start, middle (half-even) and end of the FX1 grid (FX-086)", "[midi]")
{
    auto e = miditest::make();
    miditest::Driver d {*e, 64};
    const int chorus = insertTypeIndex(*e, "chorus");
    REQUIRE(chorus >= 0);
    const auto rateId = miditest::id("fx1.chorus.rate");
    const int k = core::fx::internalIndexTable(core::builtinParams())[rateId.value];
    const auto& t = miditest::table();
    const auto g = t.fxGrid[rateId.value], gs = t.fxGrid[miditest::id("fx1.chorus.rate.div").value];
    auto stored = [&] { return e->fxBank().stored(engine::fx::insertSlot(0, 0), chorus, k); };
    auto cc = [&](int c, int v) {
        push(*e, 0xB0, static_cast<std::uint8_t>(c), static_cast<std::uint8_t>(v), static_cast<double>(d.frame()) - 1);
        d.blocks(2);
    };
    // Off slot: ignored.
    const float before = stored();
    cc(46, 100);
    CHECK(stored() == before);
    d.param("fx1.type", 0, static_cast<float>(chorus + 1));
    cc(46, 0);
    CHECK(stored() == 0.0f);
    cc(46, 127);
    CHECK(stored() == 1.0f);
    cc(46, 64);
    const float mid = stored();
    CHECK(mid == core::fx::ccToValue(64, core::fx::Grid {g.lo, g.hi}));
    const int span = g.hi - 1 - g.lo;
    CHECK(std::fabs(mid - 0.5f) <= 0.5f / static_cast<float>(span) + 1e-6f); // the middle step (half to even)
    // SYNC on: the same position addresses the knob's sync grid (the `.div` form).
    d.param("fx1.chorus.sync", 0, 1.0f);
    cc(46, 64);
    CHECK(stored() == core::fx::ccToValue(64, core::fx::Grid {gs.lo, gs.hi}));
    // An empty page position (chorus page position 5 is "") is ignored; FX2 Off is ignored.
    const auto ui = drainUi(*e);
    CHECK(std::any_of(ui.begin(), ui.end(), [&](const auto& u) {
        return u.kind == core::midi::UiEvent::Kind::Param && u.param == miditest::id("fx1.chorus.rate.div").value;
    }));
    cc(50, 99);
    cc(63, 99);
    for (const auto& u : drainUi(*e))
        CHECK(u.kind != core::midi::UiEvent::Kind::Param);
}

TEST_CASE("midi.engine: fixed CC map, scene/pattern select, all notes off (MID-023/030, SCN-050/051)", "[midi]")
{
    auto e = miditest::make();
    miditest::Driver d {*e, 64};
    drainUi(*e);
    push(*e, 0xB2, 7, 127, static_cast<double>(d.frame()) - 1); // channel 3 = track 3's volume
    d.blocks(2);
    auto ui = drainUi(*e);
    REQUIRE(! ui.empty());
    CHECK(ui[0].kind == core::midi::UiEvent::Kind::Param);
    CHECK(ui[0].param == miditest::id("track.volume").value);
    CHECK(ui[0].scope == 2);
    CHECK(ui[0].value == 127.0f);
    // CC 0 = 3 → scene 3 queued (keeps pattern 1); the switcher shows it pending.
    push(*e, 0xB0, 0, 3, static_cast<double>(d.frame()) - 1);
    d.blocks(2);
    const auto ph = e->bridge().playhead(); // stopped: the switch may land at once
    CHECK((static_cast<int>(ph.pendingScene) == 2 || static_cast<int>(ph.scene) == 2));
    ui = drainUi(*e);
    CHECK(std::any_of(ui.begin(), ui.end(), [](const auto& u) { return u.kind == core::midi::UiEvent::Kind::SceneSelect && u.scope == 2 * 16; }));
    // SCN-051: blocked while SAMPLING/EDIT is open; 0 is ignored.
    d.param("midi.remoteSelectBlocked", 0, 1.0f);
    push(*e, 0xB0, 32, 5, static_cast<double>(d.frame()) - 1);
    d.blocks(2);
    d.param("midi.remoteSelectBlocked", 0, 0.0f);
    push(*e, 0xB0, 32, 0, static_cast<double>(d.frame()) - 1);
    d.blocks(2);
    for (const auto& u : drainUi(*e))
        CHECK(u.kind != core::midi::UiEvent::Kind::SceneSelect);
    // CC 123: a Stop for every track on that channel.
    d.param("midi.in.channel", 9, 4.0f);
    d.hits.clear();
    push(*e, 0xB3, 123, 0, static_cast<double>(d.frame()) - 1);
    d.blocks(2);
    std::vector<int> stopped;
    for (const auto& h : d.hits)
        if (h.kind == TriggerKind::Stop)
            stopped.push_back(h.track);
    CHECK(stopped == std::vector<int> {3, 9});
}

TEST_CASE("midi.engine: CC 123 ALL NOTE OFF stops at its own frame (MID-023)", "[midi]")
{
    miditest::Sample sample;
    auto e = miditest::make();
    miditest::Driver d{*e, 64};
    sample = miditest::installSample(*e, 0, 4 * 44100);
    d.blocks(1);
    // One block: track 1 at +10, ALL NOTE OFF on channel 1 at +20 (tracks 1 and 2: track 2 receives channel 1 too),
    // track 1 again at +30.
    d.param("midi.in.channel", 1, 1.0f);
    const double block = static_cast<double>(d.frame()) - 64; // arrived during the previous period → this block
    push(*e, 0x90, 0, 100, block + 10);                       // track 1, note on at +10
    push(*e, 0xB0, 123, 0, block + 20);                       // ALL NOTE OFF at +20
    push(*e, 0x90, 0, 100, block + 30);                       // track 1 again at +30: after the CC, it plays
    d.hits.clear();
    d.blocks(1);
    REQUIRE(d.hits.size() == 4);
    const std::int64_t start = d.frame() - 64;
    CHECK((d.hits[0].kind == TriggerKind::Full && d.hits[0].frame == start + 10));
    CHECK((d.hits[1].kind == TriggerKind::Stop && d.hits[1].frame == start + 20 && d.hits[1].track == 0));
    CHECK((d.hits[2].kind == TriggerKind::Stop && d.hits[2].frame == start + 20 && d.hits[2].track == 1));
    CHECK((d.hits[3].kind == TriggerKind::Full && d.hits[3].frame == start + 30));
    d.blocks(8);
    CHECK(e->trackSource(0).voice().mainActive()); // the note after the CC sounds
    // A note before the CC in the same block is stopped (it never sounds past the 2 ms fade).
    push(*e, 0xB0, 123, 0, static_cast<double>(d.frame()) - 64 + 1);
    d.blocks(8);
    CHECK_FALSE(e->trackSource(0).voice().mainActive());
    push(*e, 0x90, 0, 100, static_cast<double>(d.frame()) - 64 + 10);
    push(*e, 0xB0, 123, 0, static_cast<double>(d.frame()) - 64 + 20);
    d.blocks(8);
    CHECK_FALSE(e->trackSource(0).voice().mainActive());
}

TEST_CASE("midi.engine: PAN (CC 10) and PITCH (CC 20) reach their centre at 64 (MID-031)", "[midi]")
{
    auto e = miditest::make();
    miditest::Driver d{*e, 64};
    drainUi(*e);
    auto applied = [&](int cc, int v)
    {
        push(*e, 0xB0, static_cast<std::uint8_t>(cc), static_cast<std::uint8_t>(v), static_cast<double>(d.frame()) - 1);
        d.blocks(2);
        float got = -1000.0f;
        for (const auto& u : drainUi(*e))
            if (u.kind == core::midi::UiEvent::Kind::Param && u.scope == 0)
                got = u.value;
        return got;
    };
    CHECK(applied(10, 0) == -64.0f);
    CHECK(applied(10, 63) == -1.0f);
    CHECK(applied(10, 64) == 0.0f);
    CHECK(applied(10, 127) == 64.0f);
    CHECK(applied(20, 0) == -24.0f);
    CHECK(applied(20, 63) == 0.0f);
    CHECK(applied(20, 64) == 0.0f);
    CHECK(applied(20, 127) == 24.0f);
    CHECK(applied(20, 96) == 12.0f); // whole semitones
}

TEST_CASE("midi.engine: PLAYBACK START / LOOP START / LENGTH (CC 21-23) span the track's sample", "[midi]")
{
    miditest::Sample longSample, shortSample;
    auto e = miditest::make();
    miditest::Driver d{*e, 64};
    drainUi(*e);
    auto applied = [&](std::uint8_t status, int cc, int v)
    {
        push(*e, status, static_cast<std::uint8_t>(cc), static_cast<std::uint8_t>(v),
             static_cast<double>(d.frame()) - 1);
        d.blocks(2);
        float got = -1.0f;
        for (const auto& u : drainUi(*e))
            if (u.kind == core::midi::UiEvent::Kind::Param)
                got = u.value;
        return got;
    };
    CHECK(applied(0xB0, 21, 64) == -1.0f);              // no sample yet: nothing to scale onto, ignored
    longSample = miditest::installSample(*e, 0, 44100); // 1 s
    shortSample = miditest::installSample(*e, 1, 1000);
    d.blocks(1);
    CHECK(applied(0xB0, 21, 0) == 0.0f);
    CHECK(applied(0xB0, 21, 1) == 347.0f);     // 44100 / 127 = 347.2
    CHECK(applied(0xB0, 21, 64) == 22224.0f);  // 64 · 44100 / 127 = 22223.6
    CHECK(applied(0xB0, 22, 127) == 44100.0f); // LOOP START at the end
    CHECK(applied(0xB0, 23, 127) == 44100.0f); // LENGTH: the whole sample
    CHECK(applied(0xB1, 23, 1) == 8.0f);       // track 2: 1000 / 127 = 7.9
    CHECK(applied(0xB1, 21, 127) == 1000.0f);
    const auto& t = miditest::table();
    for (int cc : {21, 22, 23})
        CHECK(t.cc[static_cast<std::size_t>(cc)].kind == core::midi::CcKind::Frames);
}

TEST_CASE("midi.engine: MIDI tracks send chords, and the hanging-note guard ends them (MID-040/047/050)", "[midi]")
{
    auto e = miditest::make();
    miditest::Driver d {*e, 64};
    auto& hub = e->midi().hub();
    hub.setOutputConnected(0, true);
    d.param("track.mode", 0, 1.0f);         // track 1 → MIDI
    d.param("midi.out.channel", 0, 5.0f);   // sends on channel 5
    d.param("midi.note2", 0, 4.0f);         // a chord: NOTE 60 and NOTE2 +4 = 64
    d.param("midi.velocity", 0, 0.0f);      // OFF: the pad's velocity
    d.param("midi.length", 0, 64.0f);       // long: still sounding when the guard runs
    d.param("seq.transpose", 0, 2.0f);      // performance transpose +2
    miditest::drainOut(*e);
    auto hit = [&] {
        e->triggers().push(engine::TriggerNow {0, 77, 60, 0}); // pad 1 (PLAY mode → track 1)
        d.blocks(2);
        e->triggers().push(engine::TriggerNow {0, 0, 60, 0});  // pad up
        d.blocks(1);
    };
    hit();
    auto out = miditest::drainOut(*e);
    REQUIRE(out.size() == 2);
    CHECK((out[0].bytes[0] == 0x94 && out[0].bytes[1] == 62 && out[0].bytes[2] == 77));
    CHECK((out[1].bytes[0] == 0x94 && out[1].bytes[1] == 66 && out[1].bytes[2] == 77));
    CHECK(e->midi().out().soundingCount() == 2);
    // Stop (play, then stop): every sounding note ends.
    e->bridge().post(engine::TransportCmd {engine::TransportCmd::Play, 0.0});
    d.blocks(2);
    e->bridge().post(engine::TransportCmd {engine::TransportCmd::Stop, 1.0});
    d.blocks(2);
    out = miditest::drainOut(*e);
    CHECK(e->midi().out().soundingCount() == 0);
    CHECK(std::count_if(out.begin(), out.end(), [](const auto& o) { return o.bytes[0] == 0x84; }) == 2);
    // Port change.
    hit();
    CHECK(e->midi().out().soundingCount() == 2);
    hub.bumpGeneration(0);
    d.blocks(1);
    CHECK(e->midi().out().soundingCount() == 0);
    // Mode change (the track leaves MIDI).
    hit();
    CHECK(e->midi().out().soundingCount() == 2);
    d.param("track.mode", 0, 0.0f);
    d.blocks(1);
    CHECK(e->midi().out().soundingCount() == 0);
    // A short note ends by itself after `length` (0.25 step at 120 BPM = 0.25 × 5512.5 = 1378.125 frames).
    d.param("track.mode", 0, 1.0f);
    d.param("midi.length", 0, 0.25f);
    d.param("midi.note2", 0, 0.0f);         // NOTE2 OFF
    miditest::drainOut(*e);
    hit();
    d.blocks(60);
    out = miditest::drainOut(*e);
    REQUIRE(out.size() == 2);
    CHECK(out[1].bytes[0] == 0x84);
    CHECK(out[1].sampleTime - out[0].sampleTime == 1378);
    // MID-050: bank MSB, LSB, program when the scene becomes current.
    d.param("midi.bankMsb", 1 << 4, 3.0f);
    d.param("midi.bankLsb", 1 << 4, 4.0f);
    d.param("midi.program", 1 << 4, 5.0f);
    d.param("track.mode", 1 << 4, 1.0f);
    miditest::drainOut(*e);
    e->bridge().post(engine::ParamChange {miditest::id("scene.queue"), 0, 16.0f, 0}); // scene 2 (stopped: at once)
    d.blocks(3);
    out = miditest::drainOut(*e);
    REQUIRE(out.size() == 3);
    CHECK((out[0].bytes[0] == 0xB4 && out[0].bytes[1] == 0 && out[0].bytes[2] == 3));
    CHECK((out[1].bytes[0] == 0xB4 && out[1].bytes[1] == 32 && out[1].bytes[2] == 4));
    CHECK((out[2].bytes[0] == 0xC4 && out[2].bytes[1] == 5));
}

TEST_CASE("midi.engine: SCALE pads of a MIDI track send SET SCALE (MIDI)'s notes; a step's PITCH lock transposes",
          "[midi][record]")
{
    auto e = miditest::make();
    miditest::Driver d {*e, 64};
    e->midi().hub().setOutputConnected(0, true);
    d.param("track.mode", 2, 1.0f);       // track 3 → MIDI
    d.param("midi.out.channel", 2, 1.0f);
    d.param("midi.note1", 2, 57.0f);      // NOTE 1 = A4
    d.param("midi.note2", 2, 4.0f);       // NOTE2 +4: a major third over NOTE
    d.param("midi.velocity", 2, 100.0f);
    d.param("scaleMidi.type", 2, 1.0f);   // IONIAN (major)
    d.param("scaleMidi.root", 2, 62.0f);  // root D5
    d.param("pads.selected", 0xFFFF, 2.0f);
    d.param("pads.mode", 0xFFFF, 4.0f);   // SCALE
    d.blocks(1);
    miditest::drainOut(*e);
    e->triggers().push(engine::TriggerNow {2, 90, 60, 2}); // pad 3 = degree 2 = F#5 (66)
    d.blocks(2);
    auto out = miditest::drainOut(*e);
    REQUIRE(out.size() == 2);
    CHECK((out[0].bytes[0] == 0x90 && out[0].bytes[1] == 66)); // NOTE 1 plays the pad's note (PAD-020)
    CHECK((out[1].bytes[0] == 0x90 && out[1].bytes[1] == 70)); // the harmony note keeps its interval
    e->triggers().push(engine::TriggerNow {2, 0, 60, 2});
    d.blocks(1);

    // A recorded step: its PITCH lock (slot 4, semitones) transposes the track's notes.
    std::array<float, 35> values {};
    values[4] = 9.0f;
    const engine::trigger::LockSet locks {1ull << 4, values.data(), nullptr};
    engine::trigger::TriggerEvent step {};
    step.track = 2;
    step.velocity = 100;
    step.kind = engine::trigger::TriggerKind::Full;
    step.source = engine::trigger::TriggerSource::Sequencer;
    step.locks = &locks;
    miditest::drainOut(*e);
    REQUIRE(e->triggerRouter().push(step));
    d.blocks(1);
    out = miditest::drainOut(*e);
    std::vector<int> on;
    for (const auto& o : out)
        if (o.bytes[0] == 0x90)
            on.push_back(o.bytes[1]);
    CHECK(on == std::vector<int> {57 + 9, 61 + 9});
}

TEST_CASE("midi.engine: a TORAIZ AS-1 track sends NOTE alone; its SCALE pads use SET SCALE (MIDI)", "[midi]")
{
    auto e = miditest::make();
    miditest::Driver d {*e, 64};
    e->midi().hub().setOutputConnected(0, true);
    d.param("track.mode", 4, 3.0f);       // track 5 → TORAIZ AS-1
    d.param("midi.note1", 4, 48.0f);      // C4
    d.param("midi.note2", 4, 7.0f);       // a fifth: the AS-1 plays no chords
    d.param("midi.velocity", 4, 100.0f);
    d.blocks(1);
    miditest::drainOut(*e);
    e->triggers().push(engine::TriggerNow {4, 90, 60, 4}); // pad 5 (PLAY)
    d.blocks(2);
    auto out = miditest::drainOut(*e);
    std::vector<int> on;
    for (const auto& o : out)
        if ((o.bytes[0] & 0xF0) == 0x90)
            on.push_back(o.bytes[1]);
    CHECK(on == std::vector<int> {48}); // NOTE only, on the track's channel
    CHECK((out.size() >= 1 && out[0].bytes[0] == 0x94)); // channel 5
    e->triggers().push(engine::TriggerNow {4, 0, 60, 4});
    d.blocks(1);

    d.param("scaleMidi.root", 4, 50.0f);  // ROOT NOTE D4: the AS-1's SCALE pads play from it (the manual's 1.4)
    d.param("pads.selected", 0xFFFF, 4.0f);
    d.param("pads.mode", 0xFFFF, 4.0f);   // SCALE (CHROMATIC)
    d.blocks(1);
    miditest::drainOut(*e);
    e->triggers().push(engine::TriggerNow {2, 90, 60, 2}); // pad 3 = root + 2 = E4
    d.blocks(2);
    on.clear();
    for (const auto& o : miditest::drainOut(*e))
        if ((o.bytes[0] & 0xF0) == 0x90)
            on.push_back(o.bytes[1]);
    CHECK(on == std::vector<int> {52});
}

TEST_CASE("midi.engine: NOTE…NOTE4 on one note number send it once", "[midi]")
{
    auto e = miditest::make();
    miditest::Driver d {*e, 64};
    e->midi().hub().setOutputConnected(0, true);
    d.param("track.mode", 1, 1.0f);   // track 2 → MIDI
    d.param("midi.note2", 1, 12.0f);
    d.param("midi.note3", 1, 12.0f);  // the same as NOTE2
    d.param("midi.note4", 1, 7.0f);
    d.blocks(1);
    miditest::drainOut(*e);
    e->triggers().push(engine::TriggerNow {1, 90, 60, 1});
    d.blocks(2);
    std::vector<int> on;
    for (const auto& o : miditest::drainOut(*e))
        if ((o.bytes[0] & 0xF0) == 0x90)
            on.push_back(o.bytes[1]);
    CHECK(on == std::vector<int> {60, 72, 67});
    CHECK(e->midi().out().soundingCount() == 3);
    // A stored NOTE bypass (an older project, an import) does not silence the track: NOTE is not bypassable.
    d.param("midi.noteBypass", 1, 1.0f);
    e->triggers().push(engine::TriggerNow {1, 0, 60, 1});
    d.blocks(1);
    miditest::drainOut(*e);
    e->triggers().push(engine::TriggerNow {1, 90, 60, 1});
    d.blocks(2);
    on.clear();
    for (const auto& o : miditest::drainOut(*e))
        if ((o.bytes[0] & 0xF0) == 0x90)
            on.push_back(o.bytes[1]);
    CHECK(on == std::vector<int> {60, 72, 67});
}

TEST_CASE("midi.engine: Active CC1 / CC2 values go out just before each trigger's notes (the manual's 2.4)", "[midi]")
{
    auto e = miditest::make();
    miditest::Driver d {*e, 64};
    e->midi().hub().setOutputConnected(0, true);
    d.param("track.mode", 2, 1.0f);       // track 3 → MIDI, channel 3
    d.param("midi.velocity", 2, 100.0f);
    d.param("midi.cc2.value1", 2, 33.0f); // CC2 PARAMETER 1 (CC 71), not Active yet
    d.param("midi.cc1.value3", 2, 20.0f); // CC1 PARAMETER 3 (CC 7)
    d.param("midi.cc1.active3", 2, 1.0f);
    d.param("midi.cc2.active1", 2, 1.0f);
    d.param("midi.cc2.value1", 2, 34.0f);
    d.param("midi.pitchBendActive", 2, 1.0f);
    d.param("midi.pitchBend", 2, -8192.0f);
    d.param("midi.aftertouch", 2, 90.0f); // aftertouch not Active: never sent
    d.blocks(2);
    CHECK(miditest::drainOut(*e).empty()); // a changed value alone sends nothing
    auto hit = [&] {
        e->triggers().push(engine::TriggerNow {2, 90, 60, 2}); // pad 3 (PLAY)
        d.blocks(2);
        e->triggers().push(engine::TriggerNow {2, 0, 60, 2});
        d.blocks(1);
        std::vector<std::vector<int>> v;
        for (const auto& o : miditest::drainOut(*e))
            if ((o.bytes[0] & 0xF0) != 0x80)
                v.push_back({o.bytes[0], o.bytes[1], o.bytes[2], static_cast<int>(o.sampleTime)});
        return v;
    };
    auto out = hit();
    REQUIRE(out.size() == 4);
    CHECK(out[0] == std::vector<int> {0xE2, 0, 0, out[3][3]});    // pitch bend −8192, at the note's frame
    CHECK(out[1] == std::vector<int> {0xB2, 7, 20, out[3][3]});   // CC1 PARAMETER 3
    CHECK(out[2] == std::vector<int> {0xB2, 71, 34, out[3][3]});  // CC2 PARAMETER 1
    CHECK((out[3][0] == 0x92 && out[3][1] == 60 && out[3][2] == 100));
    // Every trigger sends them again; a bypassed module does not.
    d.param("midi.cc2Bypass", 2, 1.0f);
    out = hit();
    REQUIRE(out.size() == 3);
    CHECK((out[0][0] == 0xE2 && out[1][1] == 7 && out[2][0] == 0x92));
    d.param("midi.cc1Bypass", 2, 1.0f); // CC1 holds pitch bend and aftertouch too
    out = hit();
    REQUIRE(out.size() == 1);
    CHECK(out[0][0] == 0x92);
    d.param("midi.cc1Bypass", 2, 0.0f);
    d.param("midi.cc2Bypass", 2, 0.0f);

    // A sequence trigger: the values at the step's frame, before its notes.
    engine::trigger::TriggerEvent step {};
    step.track = 2;
    step.velocity = 100;
    step.frame = 17;
    step.kind = engine::trigger::TriggerKind::Full;
    step.source = engine::trigger::TriggerSource::Sequencer;
    miditest::drainOut(*e);
    REQUIRE(e->triggerRouter().push(step));
    d.blocks(1);
    auto seq = miditest::drainOut(*e);
    std::erase_if(seq, [](const auto& o) { return (o.bytes[0] & 0xF0) == 0x80; }); // the pad's note ending
    REQUIRE(seq.size() == 4);
    CHECK((seq[0].bytes[0] == 0xE2 && seq[1].bytes[0] == 0xB2 && seq[2].bytes[0] == 0xB2 && seq[3].bytes[0] == 0x92));
    CHECK((seq[0].sampleTime == seq[3].sampleTime && seq[2].sampleTime == seq[3].sampleTime));

    // OUT NOTE/CC DISABLE: neither the values nor the notes.
    d.param("midi.out.noteCc", 0, 0.0f);
    CHECK(hit().empty());
    d.param("midi.out.noteCc", 0, 1.0f);

    // A TORAIZ AS-1 track sends them too, on its channel.
    d.param("track.mode", 4, 3.0f);
    d.param("midi.out.channel", 4, 9.0f);
    d.param("midi.cc2.active2", 4, 1.0f);
    d.param("midi.cc2.value2", 4, 5.0f);
    miditest::drainOut(*e);
    e->triggers().push(engine::TriggerNow {4, 90, 60, 4});
    d.blocks(2);
    auto as1 = miditest::drainOut(*e);
    std::erase_if(as1, [](const auto& o) { return (o.bytes[0] & 0xF0) == 0x80; });
    REQUIRE(as1.size() == 2);
    CHECK((as1[0].bytes[0] == 0xB8 && as1[0].bytes[1] == 72 && as1[0].bytes[2] == 5));
    CHECK(as1[1].bytes[0] == 0x98);
}

namespace
{
// Follows a 120 BPM clock (Start, then ticks) for `bars`; returns the max |phase error| in samples from bar 4 on and
// the mean error of bars 4–34 and 34–64 (drift).
struct PhaseResult
{
    double maxErr = 0, meanEarly = 0, meanLate = 0;
    double shownBpm = 0;
};
PhaseResult followClock(double jitterMs, int bars)
{
    auto e = miditest::make(kRate);
    miditest::Driver d {*e, 64};
    d.param("midi.syncSource", 0, 1.0f);
    d.param("midi.in.clock", 0, 1.0f);
    d.param("midi.in.startStop", 0, 1.0f);
    const double P = 60.0 * kRate / (24.0 * 120.0); // 918.75 frames per tick
    const double t0 = static_cast<double>(d.frame()) + 48.0 * P + 5000.0; // the pre-roll clock starts after frame 0
    std::mt19937 rng(3);
    std::uniform_real_distribution<double> jit(-jitterMs, jitterMs);
    const int ticks = bars * 96 + 96;
    std::vector<double> at(static_cast<std::size_t>(ticks));
    for (int n = 0; n < ticks; ++n)
        at[static_cast<std::size_t>(n)] = t0 + n * P + jit(rng) * kRate / 1000.0;
    // Some clock before the Start (the follower learns the tempo; no position yet).
    const double pre = t0 - 48 * P;
    int next = 0, preNext = 0;
    bool started = false;
    PhaseResult r;
    double sumE = 0, sumL = 0;
    int nE = 0, nL = 0;
    const double L = e->midi().inputLatency();
    while (next < ticks - 96)
    {
        const double now = static_cast<double>(d.frame());
        while (preNext < 48 && pre + preNext * P < now)
            push(*e, 0xF8, 0, 0, pre + (preNext++) * P);
        if (! started && t0 - P / 2 < now)
        {
            push(*e, 0xFA, 0, 0, t0 - P / 2);
            started = true;
        }
        while (started && next < ticks && at[static_cast<std::size_t>(next)] < now)
            push(*e, 0xF8, 0, 0, at[static_cast<std::size_t>(next++)]);
        d.blocks(1);
        const double F = static_cast<double>(d.frame());
        const double target = (F - (t0 + L)) / (24.0 * P); // the clock's position at frame F (ideal, jitter-free)
        const double ppq = e->bridge().playhead().ppq;
        const double bar = target / 4.0;
        if (bar >= 4.0 && bar < bars)
        {
            const double err = (ppq - target) * 24.0 * P;
            r.maxErr = std::max(r.maxErr, std::fabs(err));
            if (bar < 34.0)
            {
                sumE += err;
                ++nE;
            }
            else
            {
                sumL += err;
                ++nL;
            }
        }
    }
    r.meanEarly = nE > 0 ? sumE / nE : 0;
    r.meanLate = nL > 0 ? sumL / nL : 0;
    r.shownBpm = e->bridge().playhead().bpm;
    return r;
}
} // namespace

TEST_CASE("midi.engine: the transport phase-locks to an external clock (SYN-012: <= 1 sample per bar)", "[midi]")
{
    const auto clean = followClock(0.0, 64);
    std::printf("midi.phase: clean 120 BPM clock, bars 4-64: max |phase error| = %.4f samples, shown %.1f BPM\n",
                clean.maxErr, clean.shownBpm);
    CHECK(clean.maxErr <= 1.0);
    CHECK(clean.shownBpm == 120.0);
    const auto jittered = followClock(1.0, 64);
    const double drift = std::fabs(jittered.meanLate - jittered.meanEarly) / 30.0;
    std::printf("midi.phase: +/-1 ms jitter: max |phase error| = %.2f samples, mean bars 4-34 = %.3f, 34-64 = %.3f "
                "(drift %.4f samples/bar), shown %.1f BPM\n",
                jittered.maxErr, jittered.meanEarly, jittered.meanLate, drift, jittered.shownBpm);
    CHECK(drift <= 1.0);
    CHECK(jittered.maxErr <= 44.1); // the jitter itself, filtered: well inside 1 ms
    CHECK(jittered.shownBpm == 120.0);
}

namespace
{
// A clock at `clockBpm` into an engine whose own tempo is `internalBpm`: Start from the clock (or none: the unit's own
// PLAY), then 8 bars of ticks; returns the playhead's quarters per minute over the last 4 bars and the shown BPM.
struct FollowResult
{
    double ppqRateBpm = 0, shownBpm = 0, ppq = 0;
    bool external = false, lostWhileRunning = true, lostAfter = false;
    double shownAfterLoss = 0, rateAfterLoss = 0;
};
FollowResult followAt(double clockBpm, double internalBpm, bool clockStart, bool playFirst = false)
{
    auto e = miditest::make(kRate);
    miditest::Driver d {*e, 64};
    d.param("transport.tempo", 0, static_cast<float>(internalBpm));
    d.param("midi.syncSource", 0, 1.0f);
    if (playFirst)
        e->bridge().post(engine::TransportCmd {engine::TransportCmd::Play, 0.0});
    d.blocks(4);
    const double P = 60.0 * kRate / (24.0 * clockBpm);
    const double t0 = static_cast<double>(d.frame()) + 2000.0;
    const int ticks = 8 * 96;
    int next = 0;
    bool started = false;
    double ppqA = -1, frameA = 0;
    FollowResult r;
    while (next < ticks)
    {
        const double now = static_cast<double>(d.frame());
        if (! started && t0 - P / 2 < now)
        {
            if (clockStart)
                push(*e, 0xFA, 0, 0, t0 - P / 2);
            else if (! playFirst)
                e->bridge().post(engine::TransportCmd {engine::TransportCmd::Play, 0.0});
            started = true;
        }
        while (next < ticks && t0 + next * P < now)
            push(*e, 0xF8, 0, 0, t0 + (next++) * P);
        d.blocks(1);
        if (ppqA < 0 && next >= 4 * 96)
        {
            ppqA = e->bridge().playhead().ppq;
            frameA = static_cast<double>(d.frame());
        }
    }
    const double ppqB = e->bridge().playhead().ppq, frameB = static_cast<double>(d.frame());
    r.ppqRateBpm = (ppqB - ppqA) / ((frameB - frameA) / kRate) * 60.0;
    r.shownBpm = e->bridge().playhead().bpm;
    r.ppq = ppqB;
    r.external = e->bridge().playhead().externalClock;
    r.lostWhileRunning = e->bridge().playhead().clockLost;
    // The clock stops (the cable pulled): lost after 500 ms, the tempo kept (CLOCK LOSS = KEEP TEMPO, SYN-013).
    d.blocks(static_cast<int>(0.6 * kRate / 64));
    const double ppqC = e->bridge().playhead().ppq, frameC = static_cast<double>(d.frame());
    d.blocks(static_cast<int>(1.0 * kRate / 64));
    r.lostAfter = e->bridge().playhead().clockLost;
    r.shownAfterLoss = e->bridge().playhead().bpm;
    r.rateAfterLoss = (e->bridge().playhead().ppq - ppqC) / ((static_cast<double>(d.frame()) - frameC) / kRate) * 60.0;
    return r;
}
} // namespace

TEST_CASE("midi.engine: the transport takes the external clock's tempo, not the project's (SYN-012)", "[midi]")
{
    for (const double clock : {90.0, 140.0, 174.0})
        for (const bool start : {true, false})
            for (const bool playFirst : {false, true})
            {
                if (start && playFirst)
                    continue;
                const auto r = followAt(clock, 120.0, start, playFirst);
                INFO("clock " << clock << " start " << start << " playFirst " << playFirst << ": playhead "
                              << r.ppqRateBpm << " BPM, shown " << r.shownBpm << ", ppq " << r.ppq);
                CHECK(std::fabs(r.ppqRateBpm - clock) < 0.5);
                CHECK(std::fabs(r.shownBpm - clock) < 0.15);
                CHECK(r.external); // the UI's MIDI badge and followed BPM
                CHECK_FALSE(r.lostWhileRunning);
                CHECK(r.lostAfter);
                CHECK(std::fabs(r.shownAfterLoss - clock) < 0.15);
                CHECK(std::fabs(r.rateAfterLoss - clock) < 0.5);
            }
}

TEST_CASE("midi.engine: the USB port takes OUT or OUT + MIDI IN only (manual 1.60 p.12)", "[midi]")
{
    STATIC_REQUIRE(core::midi::legalPortMode(0, 3) == 3);
    STATIC_REQUIRE(core::midi::legalPortMode(2, 4) == 4);
    STATIC_REQUIRE(core::midi::legalPortMode(0, 9) == 4);
    auto e = miditest::make(kRate);
    miditest::Driver d{*e, 64};
    const int want[5] = {0, 1, 0, 1, 0}; // OUT, OUT+MIDI IN, (USB IN) → OUT, THRU (MIDI IN) → OUT+MIDI IN, (USB) → OUT
    for (int mode = 0; mode < 5; ++mode)
    {
        d.param("midi.portMode", 1, static_cast<float>(mode));
        CHECK(e->midi().hub().portMode(1) == want[mode]);
        d.param("midi.portMode", 0, static_cast<float>(mode)); // DIN: all five
        CHECK(e->midi().hub().portMode(0) == mode);
    }
}
