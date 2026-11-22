// The device-independent parts: the logical control map (ControlNames), the pad velocity curve (UI-114/115) and the
// lamp rules (UI-150…161).
#include "io/controllers/ControlNames.h"
#include "io/controllers/LampRules.h"
#include "io/controllers/PadCurve.h"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

using Kind = io::ControlEvent::Kind;

TEST_CASE("controls: the logical control map covers §2.1")
{
    const auto inputs = io::referenceInputControls();
    CHECK(inputs.size() == 16 + 16 + 4 + 4 + 1 + 3 + 2 + 6 + 2 + 2 + 1 + 2 + 1 + 4 + 1 + 1);
    std::set<io::Control> unique(inputs.begin(), inputs.end());
    CHECK(unique.size() == inputs.size());
    for (const auto c : inputs)
    {
        INFO(io::nameOf(c));
        CHECK(io::controlInfo(c).reference);
        CHECK(io::controlInfo(c).kind != io::ControlKind::LampOnly);
        CHECK_FALSE(io::nameOf(c).empty());
    }
    CHECK(io::controlInfo(io::pad(3)).kind == io::ControlKind::Pad);
    CHECK(io::controlInfo(io::pad(3)).lamp == io::LampKind::Rgb);
    CHECK(io::controlInfo(io::Control::Shift).lamp == io::LampKind::None);
    CHECK(io::controlInfo(io::Control::MainVolume).maxValue == 1023);
    CHECK(io::controlInfo(io::Control::Strip).maxValue == 127);
    CHECK(io::controlInfo(io::Control::PadModeMute).group == 1);
    CHECK(io::controlInfo(io::Control::StripModeUser2).group == 2);
    CHECK_FALSE(io::controlInfo(io::Control::ExtUndo).reference);
    // lamps: 16 pads, 16 steps, 4 bar keys + 4 position lamps, 4 pad modes, rec/stop/play, scene/pattern, hold,
    // active, 8 strip LEDs, 4 mode indicators, over
    CHECK(io::lampControls().size() == 16 + 16 + 4 + 4 + 4 + 3 + 2 + 1 + 1 + 8 + 4 + 1);
    CHECK(io::lampName(io::barPositionLamp(3)) == "bar.position.3");
    CHECK(io::isBarPositionLamp(io::barPositionLamp(4)));
    CHECK_FALSE(io::isBarPositionLamp(io::Control::Bar4));
}

TEST_CASE("controls: describe() is the INPUT log form")
{
    char text[64];
    io::describe({Kind::Press, io::Control::Play, 0, io::kSourcePanel, 0}, text, sizeof text);
    CHECK(std::string(text) == "play press 0");
    io::describe({Kind::Turn, io::knob(3), -2, io::kSourcePanel, 0}, text, sizeof text);
    CHECK(std::string(text) == "enc.3 turn -2");
    io::ControlEvent touch {Kind::Touch, io::Control::Screen, 1, io::kSourcePanel, 0};
    touch.x = 412;
    touch.y = 96;
    io::describe(touch, text, sizeof text);
    CHECK(std::string(text) == "screen touch 1 @412,96");
    char tiny[6];
    CHECK(io::describe({Kind::Value, io::Control::Strip, 90, io::kSourceStrip, 0}, tiny, sizeof tiny) == 5);
    CHECK(std::string(tiny) == "strip");
}

TEST_CASE("controls pad curve: Linear inverts the emulator's velocity model exactly")
{
    const io::PadCurve curve;
    for (int v = 1; v <= 127; ++v)
    {
        INFO("velocity " << v);
        CHECK(curve.velocity(io::PadCurve::emulatorWord(v)) == v);
    }
    CHECK(curve.velocity(168) == 0);
    CHECK(curve.velocity(169) == 1);
    CHECK(curve.velocity(1023) == 127);
    int last = 0;
    for (int w = 0; w <= 1023; ++w)
    {
        CHECK(curve.velocity(w) >= last);
        last = curve.velocity(w);
    }
    CHECK(curve.pressure(169) == 0);
    CHECK(curve.pressure(874) == 127);
    CHECK(curve.pressure(-5) == 0);
    CHECK(curve.pressure(5000) == 127);
}

TEST_CASE("controls pad curve: Hard/Soft shapes, fixed velocity, threshold and sensitivity")
{
    const int mid = io::PadCurve::emulatorWord(64);
    const io::PadCurve hard({}, {io::PadCurveShape::Hard3}), soft({}, {io::PadCurveShape::Soft3}),
        hard1({}, {io::PadCurveShape::Hard1}), linear;
    CHECK(hard.velocity(mid) < hard1.velocity(mid));
    CHECK(hard1.velocity(mid) < linear.velocity(mid));
    CHECK(linear.velocity(mid) < soft.velocity(mid));
    for (const auto* c : {&hard, &soft})
    {
        CHECK(c->velocity(io::PadCurve::emulatorWord(1)) == 1);
        CHECK(c->velocity(io::PadCurve::emulatorWord(127)) == 127);
    }
    const io::PadCurve fixed({}, {io::PadCurveShape::Fixed, 1, 8, 90});
    CHECK(fixed.velocity(300) == 90);
    CHECK(fixed.velocity(100) == 0);
    const io::PadCurve strict({}, {io::PadCurveShape::Linear, 4});
    CHECK(strict.velocity(169 + 23) == 0);
    CHECK(strict.velocity(169 + 24) > 0);
    const io::PadCurve sensitive({}, {io::PadCurveShape::Linear, 1, 16});
    CHECK(sensitive.velocity(mid) > linear.velocity(mid));
}

TEST_CASE("controls lamp rules: states, flash, blinks and brightness (UI-150, 151, 159)")
{
    using io::LampMode;
    CHECK(io::resolve({LampMode::Off, 0xFF0000, 0}, 0).level == 0);
    CHECK(io::resolve({LampMode::On, 0xFF0000, 0}, 0).level == 255);
    CHECK(io::resolve({LampMode::Dim, 0xFF0000, 0}, 0).level == io::kDimLevel);
    CHECK(io::resolve({LampMode::On, 0, 0}, 0).level == 0); // black is off
    const io::LampState slow {LampMode::BlinkSlow, 0xFFFFFF, 1000};
    CHECK(io::resolve(slow, 1100).level == 255);
    CHECK(io::resolve(slow, 1300).level == 0);
    CHECK(io::resolve(slow, 1500).level == 255);
    const io::LampState fast {LampMode::BlinkFast, 0xFFFFFF, 0};
    CHECK(io::resolve(fast, 50).level == 255);
    CHECK(io::resolve(fast, 100).level == 0);
    const io::LampState flash {LampMode::Flash, 0x0000FF, 0};
    CHECK(io::resolve(flash, 0).rgb == 0xFFFFFF);
    const auto half = io::resolve(flash, 500).rgb;
    CHECK(((half >> 16) > 0x40 && (half >> 16) < 0xC0 && (half & 0xFF) == 0xFF));
    CHECK(io::resolve(flash, 1000).rgb == 0x0000FF);
    CHECK(io::resolve({LampMode::On, 0xFFFFFF, 0}, 0, io::LampBrightness::Dimmed1).level == 128);
    CHECK(io::resolve({LampMode::On, 0xFFFFFF, 0}, 0, io::LampBrightness::Dimmed3).level == 64);
}

TEST_CASE("controls lamp rules: mode, strip, pad, step and bar rules (UI-152…161)")
{
    io::LampFrame f;
    io::ModeLamps m {io::PadMode::Mute, true, false, true, false, false, true, true, true, false, io::StripMode::User1};
    io::modeLamps(m, f);
    CHECK(f[io::Control::PadModeMute].mode == io::LampMode::On);
    CHECK(f[io::Control::PadModeTrack].mode == io::LampMode::Off);
    CHECK(f[io::Control::Rec].rgb == io::lampcolour::red);
    CHECK(f[io::Control::Play].rgb == io::lampcolour::green);
    CHECK(f[io::Control::Pattern].mode == io::LampMode::On);
    CHECK(f[io::Control::Scene].mode == io::LampMode::Off);
    CHECK(f[io::Control::StripModeUser1].mode == io::LampMode::On);
    CHECK(f[io::Control::StripModePitch].mode == io::LampMode::Off);
    io::modeLamps({io::PadMode::None, false, true, false, true}, f);
    CHECK(f[io::Control::Rec].mode == io::LampMode::BlinkSlow);  // waiting for the pre-count
    CHECK(f[io::Control::Play].mode == io::LampMode::BlinkSlow); // paused

    auto led = [](int i) { return static_cast<io::Control>(static_cast<int>(io::Control::StripLed1) + i); };
    io::stripLamps(70, true, false, io::StripMode::Pitch, f); // segment 4: LEDs 1…5
    for (int i = 0; i < 8; ++i)
        CHECK((f[led(i)].mode == io::LampMode::On) == (i <= 4));
    io::stripLamps(70, false, true, io::StripMode::Repeat, f); // held, one segment
    for (int i = 0; i < 8; ++i)
        CHECK((f[led(i)].mode == io::LampMode::On) == (i == 4));
    io::stripLamps(70, false, false, io::StripMode::Pitch, f);
    CHECK(f[io::Control::StripLed1].mode == io::LampMode::Off);

    io::PadLamp pad {true, 0xFF7E72};
    CHECK(io::padLamp(io::PadMode::None, pad).mode == io::LampMode::On);
    CHECK(io::padLamp(io::PadMode::None, {}).mode == io::LampMode::Off); // empty track dark
    pad.muted = true;
    CHECK(io::padLamp(io::PadMode::Mute, pad).mode == io::LampMode::Faint); // just a small light (hardware feedback)
    CHECK(io::padLamp(io::PadMode::None, pad).mode == io::LampMode::Faint);  // in every mode that shows the tracks
    CHECK(io::resolve(io::padLamp(io::PadMode::Mute, pad), 0).level == io::kFaintLevel);
    CHECK(io::resolve(io::padLamp(io::PadMode::Mute, pad), 0, io::LampBrightness::Dimmed3).level >= 1);
    pad.muted = false;
    pad.lastHitMs = 100;
    CHECK(io::padLamp(io::PadMode::None, pad).mode == io::LampMode::Flash);
    CHECK(io::padLamp(io::PadMode::Track, pad).mode == io::LampMode::Dim);

    std::array<std::uint8_t, 16> trig {};
    trig[0] = 1;
    trig[1] = 2;
    trig[2] = 3;
    io::sequenceStepLamps(trig, 0x00FF00, 4, f);
    CHECK((f[io::step(1)].mode == io::LampMode::On && f[io::step(1)].rgb == 0x00FF00));
    CHECK(f[io::step(2)].mode == io::LampMode::Dim);
    CHECK((f[io::step(3)].mode == io::LampMode::Dim && f[io::step(3)].rgb == io::lampcolour::white));
    CHECK((f[io::step(4)].mode == io::LampMode::On && f[io::step(4)].rgb == io::lampcolour::white)); // playhead
    CHECK(f[io::step(5)].mode == io::LampMode::Off);

    std::array<io::SlotState, 16> slots {};
    std::array<std::uint32_t, 16> colours {};
    colours.fill(0x8040FF);
    slots[0] = io::SlotState::Current;
    slots[1] = io::SlotState::Recorded;
    slots[2] = io::SlotState::Pending;
    io::slotStepLamps(slots, colours, 0, f);
    CHECK(f[io::step(1)].mode == io::LampMode::On);
    CHECK(f[io::step(2)].mode == io::LampMode::Dim);
    CHECK(f[io::step(3)].mode == io::LampMode::BlinkFast);
    CHECK(f[io::step(4)].mode == io::LampMode::Off);

    io::lengthModeLamps(2, f);
    CHECK(f[io::step(16)].rgb == io::lampcolour::white);
    CHECK((f[io::Control::Bar2].mode == io::LampMode::On && f[io::Control::Bar2].rgb == io::lampcolour::blue));
    CHECK(f[io::Control::Bar3].mode == io::LampMode::Off);

    io::barLamps(3, 2, 3, true, 0.25, f);
    CHECK(f[io::Control::Bar1].mode == io::LampMode::Dim);
    CHECK(f[io::Control::Bar2].mode == io::LampMode::On);
    CHECK(f[io::Control::Bar4].mode == io::LampMode::Off);
    CHECK(f[io::barPositionLamp(3)].mode == io::LampMode::On);
    io::barLamps(3, 2, 3, true, 0.75, f);
    CHECK(f[io::barPositionLamp(3)].mode == io::LampMode::Off); // pulses on the beat
}

TEST_CASE("controls lamp rules: apply() sends every lamp to the sink, then flushes")
{
    struct Sink final : io::LampSink
    {
        int lamps = 0, flushes = 0;
        void setLamp(io::Control, std::uint32_t, std::uint8_t) override { ++lamps; }
        void flush() override { ++flushes; }
    } sink;
    io::LampFrame f;
    io::apply(f, 0, io::LampBrightness::Normal, sink);
    CHECK(sink.lamps == static_cast<int>(io::lampControls().size()));
    CHECK(sink.flushes == 1);
}
