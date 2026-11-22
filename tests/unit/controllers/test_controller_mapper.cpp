// The reader-thread mapper and the controller source (control-surface.md UI-131…135, UI-118): synthetic
// MIDI streams through Generic 16 + 8 → the expected ControlEvents, TriggerNow pushes and lamp messages; the relative
// and absolute encodings of UI-132; the strip's contact rules; MMC through the raw-byte scanner; the pad bank; the
// velocity curve; MIDI learn capture; lamp diffs, nearest colours and the 60 Hz limit; and a pad note reaching the
// engine within the same audio block.
#include "io/controllers/MidiControlSource.h"
#include "io/controllers/profiles/LampEncoder.h"
#include "io/controllers/profiles/Mapper.h"
#include "io/controllers/profiles/Profile.h"

#include "tests/unit/engine/EngineTestUtil.h"
#include "tests/unit/voice/VoiceHarness.h"

#include "engine/voice/VoiceSwaps.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace ctl = io::controllers;
using io::Control;
using Kind = io::ControlEvent::Kind;

namespace
{
struct Sink final : engine::TriggerSink
{
    std::vector<engine::TriggerNow> pushed;
    bool push(const engine::TriggerNow& t) noexcept override
    {
        pushed.push_back(t);
        return true;
    }
};

const ctl::LoadedProfile& generic() { return ctl::builtinProfiles()[0]; }
const ctl::LoadedProfile& reference() { return ctl::builtinProfiles()[1]; }

ctl::LoadedProfile parse(const std::string& in, const std::string& extra = "")
{
    auto r = ctl::parseProfile(R"({"schema": 1, "id": "t", "channel": 1)" + extra + R"(, "in": [)" + in + "]}");
    REQUIRE(r.ok());
    return r.value();
}

// Feeds one message, returns the events.
struct Feed
{
    ctl::ControllerMapper m;
    Sink sink;
    bool consumed = false;
    explicit Feed(const ctl::LoadedProfile& p) : m(p, io::kSourceMidi) {}
    std::vector<io::ControlEvent> operator()(std::uint8_t a, std::uint8_t b = 0, std::uint8_t c = 0, int len = 3)
    {
        const std::uint8_t msg[3] = {a, b, c};
        ctl::MapResult r;
        consumed = m.map(msg, len, 1.0, r, &sink);
        return {r.events.begin(), r.events.begin() + r.count};
    }
};

bool is(const io::ControlEvent& e, Kind k, Control c, int v)
{
    return e.kind == k && e.control == c && e.value == v;
}
} // namespace

TEST_CASE("a synthetic Generic 16 + 8 stream yields the expected events, triggers and consumption",
          "[controllers]")
{
    Feed f(generic());
    auto e = f(0x99, 36, 100); // pad 1, channel 10
    REQUIRE(e.size() == 1);
    CHECK(is(e[0], Kind::Press, io::pad(1), 100)); // Linear curve: the MIDI velocity
    CHECK(e[0].source == io::kSourceMidi);
    REQUIRE(f.sink.pushed.size() == 1); // UI-118: straight into the engine
    CHECK(f.sink.pushed[0].track == 0);
    CHECK(f.sink.pushed[0].velocity == 100);
    CHECK(f.sink.pushed[0].pad == 0);
    e = f(0xA9, 36, 77); // poly aftertouch on the held pad
    REQUIRE(e.size() == 1);
    CHECK(is(e[0], Kind::Pressure, io::pad(1), 77));
    e = f(0x89, 36, 0);
    REQUIRE(e.size() == 1);
    CHECK(is(e[0], Kind::Release, io::pad(1), 0));
    e = f(0x99, 51, 0); // note on velocity 0 = off (nothing was down)
    CHECK(e.empty());
    CHECK(f.consumed);
    CHECK(f.sink.pushed.size() == 1); // releases never trigger

    CHECK(is(f(0xB9, 72, 0x02)[0], Kind::Turn, io::knob(3), 2));   // two's complement up
    CHECK(is(f(0xB9, 75, 0x7E)[0], Kind::Turn, io::knob(6), -2));  // … and down
    CHECK(is(f(0xB9, 76, 0x01)[0], Kind::Turn, Control::Selector, 1));
    CHECK(is(f(0xB9, 77, 0x01)[0], Kind::Value, Control::MainVolume, 520)); // 512 + 8 per detent
    CHECK(is(f(0xB9, 77, 0x7F)[0], Kind::Value, Control::MainVolume, 512));
    CHECK(is(f(0x99, 52, 127)[0], Kind::Press, Control::SelectorPush, 0));
    CHECK(is(f(0x89, 52, 0)[0], Kind::Release, Control::SelectorPush, 0));
    CHECK(is(f(0x99, 53, 1)[0], Kind::Press, Control::Back, 0));
    CHECK(is(f(0x99, 54, 1)[0], Kind::Press, Control::Home, 0));
    CHECK(is(f(0xB9, 115, 127)[0], Kind::Press, Control::Play, 0));
    CHECK(f(0xB9, 115, 127).empty()); // still held: no second press
    CHECK(is(f(0xB9, 115, 0)[0], Kind::Release, Control::Play, 0));
    CHECK(is(f(0xB9, 118, 64)[0], Kind::Press, Control::Shift, 0));
    CHECK(is(f(0xB9, 118, 63)[0], Kind::Release, Control::Shift, 0));

    // Unbound: another channel, an unused CC, a program change — not consumed (they stay MIDI input, UI-136).
    CHECK(f(0x90, 36, 100).empty());
    CHECK_FALSE(f.consumed);
    CHECK(f(0xB9, 20, 1).empty());
    CHECK_FALSE(f.consumed);
    CHECK(f(0xC9, 3, 0, 2).empty());
    CHECK_FALSE(f.consumed);
    CHECK(f(0xF8, 0, 0, 1).empty());
    CHECK_FALSE(f.consumed);
}

TEST_CASE("the relative and absolute encodings of UI-132", "[controllers]")
{
    Feed f(parse(R"({"cc": 1, "enc": "twos", "control": "enc.1"}, {"cc": 2, "enc": "offset", "control": "enc.2"},
                    {"cc": 3, "enc": "signmag", "control": "enc.3"}, {"cc": 4, "control": "enc.4", "scale": 2},
                    {"cc": 5, "control": "afs.drive"}, {"pitchbend": true, "control": "afs.cutoff"},
                    {"pressure": true, "control": "enc.5"})"));
    CHECK(f(0xB0, 1, 1)[0].value == 1);
    CHECK(f(0xB0, 1, 63)[0].value == 63);
    CHECK(f(0xB0, 1, 65)[0].value == -63);
    CHECK(f(0xB0, 1, 127)[0].value == -1);
    CHECK(f(0xB0, 2, 66)[0].value == 2);
    CHECK(f(0xB0, 2, 60)[0].value == -4);
    CHECK(f(0xB0, 2, 64).empty());
    CHECK(f(0xB0, 3, 5)[0].value == 5);
    CHECK(f(0xB0, 3, 64 + 5)[0].value == -5);
    // Absolute → detents by difference: the first value is only the reference; 127 → 0 is a wrap (ignored).
    CHECK(f(0xB0, 4, 10).empty());
    CHECK(f(0xB0, 4, 13)[0].value == 6); // 3 × scale 2
    CHECK(f(0xB0, 4, 11)[0].value == -4);
    CHECK(f(0xB0, 4, 127).empty()); // a jump of ≥ 64
    CHECK(f(0xB0, 4, 0).empty());
    CHECK(f(0xB0, 4, 1)[0].value == 2);
    // Absolute controls: 0–127 → 0–1023, pitch bend 14 bit → 0–1023.
    CHECK(is(f(0xB0, 5, 127)[0], Kind::Value, Control::AfsDrive, 1023));
    CHECK(is(f(0xB0, 5, 0)[0], Kind::Value, Control::AfsDrive, 0));
    CHECK(is(f(0xE0, 0x7F, 0x7F)[0], Kind::Value, Control::AfsCutoff, 1023));
    CHECK(is(f(0xE0, 0x00, 0x40)[0], Kind::Value, Control::AfsCutoff, 511));
    // Channel pressure on an encoder: by difference too.
    CHECK(f(0xD0, 20, 0, 2).empty());
    CHECK(f(0xD0, 25, 0, 2)[0].value == 5);
}

TEST_CASE("the strip from pitch bend and the contact CC", "[controllers]")
{
    Feed f(generic());
    auto e = f(0xE9, 0x00, 0x40); // centre: no contact
    CHECK(e.empty());
    e = f(0xE9, 0x00, 0x60); // off centre: touch at 96
    REQUIRE(e.size() == 1);
    CHECK(is(e[0], Kind::Touch, Control::Strip, 96));
    e = f(0xE9, 0x00, 0x50);
    CHECK(is(e[0], Kind::Value, Control::Strip, 80));
    e = f(0xE9, 0x00, 0x40); // back to centre: release
    CHECK(is(e[0], Kind::Release, Control::Strip, 64));
    // The contact CC holds the strip down at the centre too, and lifting it releases.
    e = f(0xB9, 119, 127);
    CHECK(is(e[0], Kind::Touch, Control::Strip, 64));
    e = f(0xE9, 0x00, 0x40);
    CHECK(is(e[0], Kind::Value, Control::Strip, 64));
    e = f(0xB9, 119, 0);
    CHECK(is(e[0], Kind::Release, Control::Strip, 64));
}

TEST_CASE("MMC through the raw bytes, realtime Start / Stop / Continue", "[controllers]")
{
    ctl::MidiControlSource gen(generic(), 0, "PADS", nullptr);
    const std::uint8_t mmcPlay[] = {0xF0, 0x7F, 0x7F, 0x06, 0x02, 0xF7, 0xF0, 0x7F, 0x10, 0x06, 0xF8, 0x01, 0xF7};
    gen.onRawBytes(mmcPlay, 7, 1.0); // a split read: the second SysEx continues in the next call
    gen.onRawBytes(mmcPlay + 7, 6, 2.0);
    std::vector<io::ControlEvent> out;
    gen.poll(out);
    REQUIRE(out.size() == 4);
    CHECK(is(out[0], Kind::Press, Control::Play, 0));
    CHECK(is(out[1], Kind::Release, Control::Play, 0));
    CHECK(is(out[2], Kind::Press, Control::Stop, 0)); // the realtime byte inside the SysEx was skipped
    const std::uint8_t other[] = {0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7}; // not MMC
    gen.onRawBytes(other, sizeof other, 3.0);
    out.clear();
    gen.poll(out);
    CHECK(out.empty());

    Feed r(reference());
    CHECK(is(r(0xFA, 0, 0, 1)[0], Kind::Press, Control::Play, 0));
    CHECK(is(r(0xFB, 0, 0, 1)[0], Kind::Press, Control::Play, 0));
    CHECK(is(r(0xFC, 0, 0, 1)[0], Kind::Press, Control::Stop, 0));
    CHECK(r(0xF8, 0, 0, 1).empty());
    // Pads arrive as the track's note on its own channel: pad n = 60 + n − 1 on any channel.
    CHECK(is(r(0x93, 63, 90)[0], Kind::Press, io::pad(4), 90));
    CHECK(r(0xB2, 71, 50).empty()); // CC2 page knob 1: reference
    CHECK(is(r(0xB2, 71, 53)[0], Kind::Turn, io::knob(1), 3));
}

TEST_CASE("the pad bank turns the pads into step keys while ext.steps is held", "[controllers]")
{
    Feed f(generic());
    CHECK(f(0x99, 55, 127).empty()); // ext.steps down: consumed, no event
    CHECK(f.consumed);
    auto e = f(0x99, 40, 100);
    REQUIRE(e.size() == 1);
    CHECK(is(e[0], Kind::Press, io::step(5), 0));
    CHECK(f.sink.pushed.empty()); // a step key never triggers
    CHECK(f(0x89, 55, 0).empty()); // ext.steps up while the pad is still down …
    e = f(0x89, 40, 0);
    CHECK(is(e[0], Kind::Release, io::step(5), 0)); // … the pad releases what it pressed
    e = f(0x99, 40, 100);
    CHECK(is(e[0], Kind::Press, io::pad(5), 100));
}

TEST_CASE("the device's pad response (curve, threshold, sensitivity, fixed) and trim", "[controllers]")
{
    io::PadResponse lin;
    auto t = ctl::velocityTable(lin);
    for (int v = 1; v < 128; ++v)
        CHECK(t[static_cast<std::size_t>(v)] == v); // Linear, threshold 1, sensitivity 8 = identity
    io::PadResponse hard {io::PadCurveShape::Hard3, 1, 8, 100};
    io::PadResponse soft {io::PadCurveShape::Soft3, 1, 8, 100};
    const auto h = ctl::velocityTable(hard), s = ctl::velocityTable(soft);
    CHECK(h[64] < 64);
    CHECK(s[64] > 64);
    CHECK(h[127] == 127);
    CHECK(s[1] >= 1);
    const auto fixed = ctl::velocityTable({io::PadCurveShape::Fixed, 1, 8, 90});
    CHECK(fixed[3] == 90);
    CHECK(fixed[127] == 90);
    const auto thr = ctl::velocityTable({io::PadCurveShape::Linear, 5, 8, 100});
    CHECK(thr[15] == 0); // below (5 − 1) · 4
    CHECK(thr[16] == 16);
    const auto sens = ctl::velocityTable({io::PadCurveShape::Linear, 1, 16, 100});
    CHECK(sens[64] == 127);

    Feed f(generic());
    f.m.setResponse({io::PadCurveShape::Fixed, 1, 8, 77});
    f.m.setTrim(0, 10);
    auto e = f(0x99, 36, 5);
    CHECK(is(e[0], Kind::Press, io::pad(1), 87));
    CHECK(f.m.lastVelocity() == 87);
    CHECK(f.sink.pushed.back().velocity == 87);
}

TEST_CASE("MIDI learn captures the next message instead of mapping it", "[controllers]")
{
    ctl::MidiControlSource src(generic(), 2, "PADS", nullptr);
    src.armLearn();
    const std::uint8_t off[] = {0x89, 36, 0};
    CHECK(src.onMidi(off, 3, 1.0)); // a note off is not learnable: mapped as usual (bound: consumed, nothing down)
    CHECK(src.learning());
    const std::uint8_t cc[] = {0xB3, 21, 1};
    CHECK(src.onMidi(cc, 3, 2.0)); // captured (and consumed) although unbound
    ctl::LearnedMessage m;
    REQUIRE(src.takeLearned(m));
    CHECK(m.status == 0xB3);
    CHECK(m.number == 21);
    CHECK_FALSE(src.learning());
    CHECK_FALSE(src.takeLearned(m)); // once
    CHECK_FALSE(src.onMidi(cc, 3, 3.0)); // learning over: unbound again
}

TEST_CASE("lamps — nearest palette colour, levels, diffs and at most one send per 15 ms", "[controllers]")
{
    const auto& g = generic();
    ctl::LampEncoder lamps(g);
    std::vector<std::vector<std::uint8_t>> sent;
    auto send = [&](const std::uint8_t* b, int n) {
        sent.emplace_back(b, b + n);
        return true;
    };
    // First flush: every lamp once (unknown device state), off.
    CHECK(lamps.flush(0.0, send) == 19);
    CHECK(sent[0] == std::vector<std::uint8_t> {0x99, 36, 0});
    sent.clear();
    lamps.set(io::pad(1), 0xFF7E72, 255); // track 1's colour → palette entry 1
    lamps.set(io::pad(2), 0x3FB3FF, 255); // track 11 → 11
    lamps.set(io::pad(3), 0xFFFFFF, 255); // white → 17
    lamps.set(io::pad(4), 0xFF7E72, 0);   // level 0 → off
    lamps.set(Control::Play, 0x28F03C, 255); // mono lamp: lit → value 127
    CHECK(lamps.flush(5.0, send) == 0); // within 15 ms of the last send: waits
    CHECK(lamps.flush(20.0, send) == 4);  // pad 4 stayed off: not sent again
    CHECK(sent[0] == std::vector<std::uint8_t> {0x99, 36, 1});
    CHECK(sent[1] == std::vector<std::uint8_t> {0x99, 37, 11});
    CHECK(sent[2] == std::vector<std::uint8_t> {0x99, 38, 17});
    CHECK(sent[3] == std::vector<std::uint8_t> {0xB9, 115, 127});
    sent.clear();
    CHECK(lamps.flush(100.0, send) == 0); // nothing changed: nothing sent
    lamps.set(io::pad(1), 0xFF7E72, 10);  // very dim → nearest is off
    CHECK(lamps.value(io::pad(1)) == 0);
    CHECK(lamps.value(io::knob(1)) == -1);
    CHECK(ctl::LampEncoder::nearest(g.palettes[0], 0xF68C36) == 3);
    lamps.resend();
    CHECK(lamps.flush(200.0, send) == 19);
}

TEST_CASE("a pad note reaches the engine's trigger queue within the same audio block (UI-118)",
          "[controllers]")
{
    voicetest::TestBuffer sample(voicetest::sine(4410, 440.0, voicetest::kRate, 0.5f));
    {
        engine::EngineOptions o;
        o.testTone = false;
        engine::Engine e(enginetest::fullSetup(), o);
        e.prepare(44100.0, 64);
        core::audio::retain(sample.get());
        REQUIRE(e.bridge().post(engine::ObjectSwap {engine::voice::trackSampleSlot(0, 0), sample.get()}));
        enginetest::render(e, 1024, 64); // installs the sample
        REQUIRE(e.voiceStarts() == 0);
        ctl::MidiControlSource src(generic(), 0, "PADS", &e.triggers());
        const std::uint8_t hit[] = {0x99, 36, 110};
        REQUIRE(src.onMidi(hit, 3, 0.0)); // the reader thread's call: pushes TriggerNow before it returns
        const auto out = enginetest::render(e, 64, 64); // the very next 64-frame block
        CHECK(e.voiceStarts() == 1);
        CHECK(enginetest::peak(out.l, 0, 64) > 0.0f);
    }
    CHECK(sample.refs() == 0);
}
