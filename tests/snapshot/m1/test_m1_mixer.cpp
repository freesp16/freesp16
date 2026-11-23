// MIXER and OUTPUT SETTING against mixer.svg / output-setting.svg with the mockup's data (mixer.py): levels, pans,
// sends, track 6 muted, track 1 selected, SEND / MASTER bus levels; plus the strip interactions (MUTE / SOLO taps,
// fader drag on the meter, rotary focus through the rail into the strips, columns 1-3 follow the selection).
#include "core/util/GainLaw.h"
#include "tests/snapshot/m1/M1Fixture.h"
#include "ui/shell/Layout.h"
#include "ui/widgets/ChannelStrip.h"
#include "ui/widgets/Meter.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace snapshot;
using m1test::M1Fixture;
namespace L = ui::layout;

namespace
{
// dB that puts a meter bar's top at `fraction` of [top, bottom] (the mockup draws levels as fractions).
float dbAt(float fraction, float top, float bottom)
{
    const float y = bottom - (bottom - top) * fraction;
    return 3.0f - (y - top) * 53.0f / (bottom - top - 12.0f);
}
float lin(float db)
{
    return std::pow(10.0f, db / 20.0f);
}
int volumeAt(float fraction)
{
    const float target =
        ui::ChannelStrip::kMeterTop + (1 - fraction) * (ui::ChannelStrip::kMeterBottom - ui::ChannelStrip::kMeterTop);
    int best = 100;
    float bestD = 1e9f;
    for (int i = 1; i <= 127; ++i)
    {
        const float d = std::fabs(ui::meterY(static_cast<float>(core::GainLaw::decibels(i)),
                                             ui::ChannelStrip::kMeterTop, ui::ChannelStrip::kMeterBottom) -
                                  target);
        if (d < bestD)
            bestD = d, best = i;
    }
    return best;
}

void mixerDemo(M1Fixture& f, bool playing = true)
{
    auto& st = f.shell->state();
    st.playing = playing;
    st.position.set("002:03:05");
    engine::MeterFeed m{};
    const struct
    {
        int t;
        float lv;
    } levels[] = {{1, 0.62f}, {2, 0.55f}, {3, 0.35f}, {5, 0.7f}, {11, 0.2f}};
    if (playing)
    {
        for (const auto& l : levels)
        {
            const float a = lin(dbAt(l.lv, 356, 560)), b = lin(dbAt(l.lv * 0.95f, 356, 560));
            m.trackPeak[static_cast<std::size_t>(l.t - 1)] = {a, b};
        }
        m.sendPeak = {lin(dbAt(0.42f, 184, 600)), lin(dbAt(0.42f * 0.96f, 184, 600))};
        m.masterPeak = {lin(dbAt(0.74f, 184, 600)), lin(dbAt(0.74f * 0.96f, 184, 600))};
    }
    f.shell->feeds().inject(m, {});
    for (int t = 1; t <= 16; ++t)
        f.set("track.volume", t, static_cast<float>(volumeAt(t == 3 ? 0.66f : t == 6 ? 0.72f : 0.79f)));
    f.set("track.volume", 1, 100); // the selected strip's VOLUME column reads 0.0 dB in the mockup
    f.set("track.pan", 3, 23);
    f.set("track.pan", 5, -13);
    f.set("track.sendAmount", 2, 44);
    f.set("track.sendAmount", 5, 64);
    f.set("track.mute", 6, 1);
}
} // namespace

TEST_CASE("ui.m1.mixer: MIXER matches mixer.svg", "[snapshot][m1]")
{
    Session session;
    M1Fixture f;
    REQUIRE(f.shell->open("mixer"));
    mixerDemo(f);
    const auto ours = f.image();
    const auto& mock = m1test::m1mockup("mixer");
    // The strips (52 on a 56 pitch, meter centred, fader marker across the bars) and the compact SEND / MASTER pair
    // depart from mixer.svg by request (hardware feedback: more room for the strips): their geometry is checked
    // below instead, the rest of the screen against the mockup.
    compare(ours, mock, L::canvas, "m1-mixer"); // artifacts only
    CHECK(compare(ours, mock, {16, 152, 192, 456}, "m1-mixer-rail").match >= kGate);
    // The strip's level values read 0–127 instead of the mockup's dB text (by request): a few glyphs differ.
    CHECK(compare(ours, mock, L::knobStrip, "m1-mixer-strip").match >= kGate - 0.01);
    CHECK(compare(ours, mock, L::contextBar, "m1-mixer-context").match >= kGate);
}

TEST_CASE("ui.m1.mixer: OUTPUT SETTING matches output-setting.svg", "[snapshot][m1]")
{
    Session session;
    M1Fixture f;
    REQUIRE(f.shell->open("mixer"));
    REQUIRE(f.shell->open("mixer.output"));
    mixerDemo(f);
    f.set("track.mute", 6, 0);
    f.shell->state().selectedTrack = 5;
    const auto ours = f.image();
    const auto& mock = m1test::m1mockup("output-setting");
    compare(ours, mock, {216, 140, 1048, 472}, "m1-output-strips"); // artifacts only: the layout departs from the mockup
    CHECK(compare(ours, mock, {16, 152, 192, 456}, "m1-output-rail").match >= kGate);
    CHECK(compare(ours, mock, L::statusRow, "m1-output-breadcrumb").match >= kGate);
}

TEST_CASE("ui.m1.mixer: the strips and the compact bus pair share the content width without overlap", "[snapshot][m1]")
{
    using ui::ChannelStrip;
    using ui::Meter;
    CHECK(ChannelStrip::kW >= 52);
    for (int t = 1; t < 16; ++t)
        CHECK(ChannelStrip::rect(t).x + ChannelStrip::kW < ChannelStrip::rect(t + 1).x);
    const auto last = ChannelStrip::rect(16);
    CHECK(last.x + last.w <= Meter::kBusX - 8); // a gutter before the bus pair
    CHECK(Meter::kBusX + Meter::kBusPitch + Meter::kBusW <= Meter::kBusScaleR - 24); // room for "-50"
    CHECK(Meter::kBusScaleR <= 1264);
    // The meter's bars (and the fader cap 4 lpx past them each side) fit inside the strip.
    CHECK((ChannelStrip::kW - ChannelStrip::kBarsW) / 2 >= 4);
}

TEST_CASE("ui.m1.mixer: taps toggle MUTE / SOLO, a meter drag moves the fader, the rotary walks rail then strips",
          "[m1]")
{
    M1Fixture f;
    REQUIRE(f.shell->open("mixer"));
    f.settle();
    auto mute = ui::ChannelStrip::muteRect(4), solo = ui::ChannelStrip::soloRect(7),
         meter = ui::ChannelStrip::meterRect(9);
    f.tap(mute.x + 24, mute.y + 22);
    CHECK(ui::m1::trackFlag(*f.shell, "track.mute", 4));
    CHECK(f.shell->state().selectedTrack == 4);
    f.tap(solo.x + 24, solo.y + 22);
    CHECK(ui::m1::trackFlag(*f.shell, "track.solo", 7));
    f.frame();
    // drag on track 9's meter above its top: the scale top is +3 dB
    io::ControlEvent e{io::ControlEvent::Kind::Touch, io::Control::Screen, ui::touch::kDown, io::kSourceVirtual, 0};
    e.x = static_cast<std::int16_t>((meter.x + 24) / ui::touch::kToCanvas);
    e.y = static_cast<std::int16_t>((meter.y + 100) / ui::touch::kToCanvas);
    f.send(e);
    // The drag reaches the body through the shell (the body took the touch's down, UID-561 / UI-209).
    e.value = ui::touch::kDrag;
    e.y = static_cast<std::int16_t>((meter.y - 40) / ui::touch::kToCanvas);
    f.send(e);
    e.value = ui::touch::kUp;
    f.send(e);
    CHECK(core::GainLaw::decibels(static_cast<int>(ui::m1::trackParam(*f.shell, "track.volume", 9))) >=
          3.0); // the top = +3 dB
    // columns 1–3 follow the selection, 4–6 are global
    f.shell->state().selectedTrack = 2;
    f.turn(io::knob(1), -10);
    CHECK(ui::m1::trackParam(*f.shell, "track.volume", 2) == 90);
    CHECK(ui::m1::trackParam(*f.shell, "track.volume", 4) == 100);
    // rotary: from strip 1 back into the rail (OUTPUT SETTING), then forward to strip 2
    f.shell->state().selectedTrack = 1;
    f.turn(io::Control::Selector, -1);
    f.turn(io::Control::Selector, 2);
    CHECK(f.shell->state().selectedTrack == 2);
    f.turn(io::Control::Selector, -2);
    f.press(io::Control::SelectorPush); // OUTPUT SETTING
    f.settle();
    CHECK(f.shell->router().current().id == "mixer.output");
    f.press(io::Control::Back);
    f.settle();
    CHECK(f.shell->router().current().id == "mixer");
}
