// OUTPUT SETTING (mixer.md "Layout — OUTPUT SETTING", UID-558…563; screens.md UI-275…278) against
// output-setting.svg (tracks 5 / 6 on OUT3-4 / OUT5-6, track 5 selected); ASSIGN MODE / HP CUE lists (the cue list per
// mode, the MIX-040 correction + Toast), OUT ASSIGN on knob 3 per mode, the THRU BYPASS column on a THRU track, the
// routing fields in the document model (undoable, posted with the scene scope), the ANALOG FILTER SETTING rows on
// afs.*, and the keyboard-only workflow W20 (UI-420).
#include "core/commands/CommandBusAccess.h"
#include "core/util/GainLaw.h"
#include "tests/fakes/FakeEngineBridge.h"
#include "tests/snapshot/m5/M5Fixture.h"
#include "ui/shell/Layout.h"
#include "ui/shell/ModelParamAccess.h"
#include "ui/widgets/ChannelStrip.h"
#include "ui/widgets/ListPopup.h"
#include "ui/widgets/Meter.h"
#include "ui/widgets/SettingPopup.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace snapshot;
using m5test::M5Fixture;
namespace L = ui::layout;

namespace
{
// The mockup's levels and faders (mixer.py, as tests/snapshot/m1/test_m1_mixer.cpp).
float dbAt(float fraction, float top, float bottom)
{
    const float y = bottom - (bottom - top) * fraction;
    return 3.0f - (y - top) * 53.0f / (bottom - top - 12.0f);
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
void outputDemo(M5Fixture& m)
{
    auto lin = [](float db) { return std::pow(10.0f, db / 20.0f); };
    engine::MeterFeed feed {};
    const struct
    {
        int t;
        float lv;
    } levels[] = {{1, 0.62f}, {2, 0.55f}, {3, 0.35f}, {5, 0.7f}, {11, 0.2f}};
    for (const auto& l : levels)
        feed.trackPeak[static_cast<std::size_t>(l.t - 1)] = {lin(dbAt(l.lv, 356, 560)),
                                                             lin(dbAt(l.lv * 0.95f, 356, 560))};
    feed.sendPeak = {lin(dbAt(0.42f, 184, 600)), lin(dbAt(0.42f * 0.96f, 184, 600))};
    feed.masterPeak = {lin(dbAt(0.74f, 184, 600)), lin(dbAt(0.74f * 0.96f, 184, 600))};
    m.shell().feeds().inject(feed, {});
    m.state().playing = true;
    m.state().position.set("002:03:05");
    for (int t = 1; t <= 16; ++t)
        m.set("track.volume", t, static_cast<float>(volumeAt(t == 3 ? 0.66f : t == 6 ? 0.72f : 0.79f)));
    m.set("track.volume", 1, 100);
    m.set("track.pan", 3, 23);
    m.set("routing.outAssign", 5, 2); // OUT3-4
    m.set("routing.outAssign", 6, 3); // OUT5-6
    m.set("track.pan", 5, -13);
    m.shell().selectTrack(5);
}
int listItems(M5Fixture& m)
{
    auto* top = dynamic_cast<ui::ListPopup*>(m.shell().topPopup());
    return top != nullptr ? top->rowCount() : -1;
}
} // namespace

TEST_CASE("snapshot.m5.output: OUTPUT SETTING (ui/screens/output) matches output-setting.svg", "[snapshot][m5]")
{
    Session session;
    M5Fixture m;
    REQUIRE(m.open("mixer"));
    REQUIRE(m.open("mixer.output"));
    outputDemo(m);
    const auto ours = m.f.image();
    CHECK(m.hasText("3-4"));
    CHECK(m.hasText("5-6"));
    CHECK(m.shell().column(2).spec->id == "routing.outAssign");
    CHECK(m.shell().boundColumn(4) == nullptr); // column 5 stays empty on a sample track
    const auto& mock = m5test::m5mockup("output-setting");
    // The strips and the compact PHONES / MASTER pair depart from the mockup by request (wider strips): artifacts only;
    // their geometry is checked in test_m1_mixer.cpp.
    compare(ours, mock, {216, 140, 1048, 472}, "m5-output-strips");
    compare(ours, mock, {16, 152, 192, 456}, "m5-output-rail"); // artifacts only: the narrow rail (by request)
    CHECK(compare(ours, mock, L::knobStrip, "m5-output-strip").match >= kGate);
    compare(ours, mock, L::canvas, "m5-output");
}

TEST_CASE("m5ui.output: HP CUE per mode, the MIX-040 correction, OUT ASSIGN per mode, THRU BYPASS column", "[m5]")
{
    M5Fixture m;
    REQUIRE(m.open("mixer.output"));
    CHECK(m.shell().current().parent == "mixer");
    m.shell().selectTrack(3);
    // OUT ASSIGN in M + 6CH: -- → OUT3-4 → OUT5-6 → OUT7-8 → 3 … (OUT1-2, 1, 2 are not offered).
    m.knob(3, 1);
    CHECK(m.get("routing.outAssign", 3) == 2);
    m.knob(3, 3);
    CHECK(m.get("routing.outAssign", 3) == 7); // mono 3
    m.knob(3, -10);
    CHECK(m.get("routing.outAssign", 3) == 0); // --
    // HP CUE (rail 4, reached by turning back from strip 1): 10 entries in M + 6CH.
    m.shell().selectTrack(1);
    m.turnSel(-1);
    REQUIRE(m.shell().railFocus() == 3);
    m.push();
    m.f.settle();
    CHECK(listItems(m) == 10);
    m.click(io::Control::Back);
    // ASSIGN MODE 8CH: the cue list grows to 13; choose OUT1-2.
    m.turnSel(-1);
    REQUIRE(m.shell().railFocus() == 2);
    m.push();
    m.turnSel(1);
    m.push();
    m.f.settle();
    CHECK(m.get("routing.assignMode") == 1);
    m.turnSel(1);
    m.push();
    m.f.settle();
    CHECK(listItems(m) == 13);
    m.turnSel(1); // MASTER → OUT1-2
    m.push();
    CHECK(m.get("routing.headphoneCue") == 1);
    // Back to M + 6CH: OUT1-2 is not offered there → MASTER and a Toast (UID-562).
    m.turnSel(-1);
    m.push();
    m.turnSel(-1);
    m.push();
    m.f.settle();
    CHECK(m.get("routing.assignMode") == 0);
    CHECK(m.get("routing.headphoneCue") == 0);
    CHECK(m.hasText("HP CUE set to MASTER"));
    // A THRU track: knob 5 binds THRU BYPASS (MIX-051).
    m.set("track.mode", 9, 2);
    m.shell().selectTrack(9);
    m.f.settle();
    REQUIRE(m.shell().boundColumn(4) != nullptr);
    CHECK(m.shell().boundColumn(4)->id == "routing.thruBypass");
    m.knob(5, 1);
    CHECK(m.get("routing.thruBypass", 9) == 1);
    m.shell().selectTrack(2);
    m.f.settle();
    CHECK(m.shell().boundColumn(4) == nullptr);
}

TEST_CASE("m5ui.output: routing values live in the document model: undoable, scene-scoped posts", "[m5]")
{
    fakes::FakeEngineBridge bridge;
    auto& session = ui::m2::Session::instance();
    session.reset();
    ui::ModelParamAccess values(session.bus(), &bridge);
    const auto& params = core::builtinParams();
    const auto* mode = params.find("routing.assignMode");
    const auto* out = params.find("routing.outAssign");
    const auto* level = params.find("routing.monitorLevel");
    REQUIRE((mode != nullptr && out != nullptr && level != nullptr));
    CHECK(values.modelBacked(*mode));
    CHECK(values.modelBacked(*out));
    CHECK(values.modelBacked(*level));
    values.set(*mode, 3, 1.0f);                            // scene 4: 8CH
    values.set(*out, static_cast<std::uint16_t>(3 << 4 | 6), 4.0f); // scene 4 track 7: OUT7-8
    values.set(*level, 0, 90.0f);
    const auto& p = session.project();
    CHECK(p.scenes[3].routing.assignMode == core::model::AssignMode::EightChannel);
    CHECK(p.scenes[0].routing.assignMode == core::model::AssignMode::MasterPlus6);
    CHECK(p.scenes[3].tracks[6].routing.output == core::model::OutputAssign::Out78);
    CHECK(p.settings.monitorVolume == 90);
    CHECK(values.get(*mode, 3) == 1.0f);
    CHECK(values.get(*mode, 0) == 0.0f);
    // The bus hook's half: the changed paths become ParamChanges with the scene scope.
    core::ChangeSet changes;
    changes.add("scene[3].routing.assignMode");
    changes.add("scene[3].track[6].routing.outAssign");
    bridge.posted.clear();
    ui::forwardModelChanges(changes, p, bridge);
    bool sawMode = false, sawOut = false;
    for (const auto& msg : bridge.posted)
    {
        const auto* pc = std::get_if<engine::ParamChange>(&msg);
        if (pc == nullptr)
            continue;
        const auto& c = *pc;
        sawMode |= c.param == *params.idOf("routing.assignMode") && c.scope == 3 && c.value == 1.0f;
        sawOut |= c.param == *params.idOf("routing.outAssign") && c.scope == (3 << 4 | 6) && c.value == 4.0f;
    }
    CHECK(sawMode);
    CHECK(sawOut);
    CHECK(session.undo()); // MONITOR LEVEL
    CHECK(session.undo()); // OUT ASSIGN
    CHECK(session.undo()); // ASSIGN MODE
    CHECK(session.project().scenes[3].routing.assignMode == core::model::AssignMode::MasterPlus6);
    session.reset();
}

TEST_CASE("m5ui.mixer_afs: ANALOG FILTER SETTING rows write afs.*; PROCESSOR EXTERNAL opens mixer.afs", "[m5]")
{
    M5Fixture m;
    REQUIRE(m.open("mixer"));
    m.shell().selectTrack(1);
    m.turnSel(-1); // strip 1 → rail OUTPUT SETTING
    m.turnSel(-3); // → ANALOG FILTER SETTING
    REQUIRE(m.shell().railFocus() == 0);
    m.push();
    m.f.settle();
    auto* pop = dynamic_cast<ui::SettingPopup*>(m.shell().topPopup());
    REQUIRE(pop != nullptr);
    CHECK(pop->row(0).label == "INSERT TRACK");
    CHECK(pop->row(1).disabled); // INSERT ORDER while MASTER
    CHECK(pop->row(4).spec != nullptr);
    // ACTIVE on: turn to row 3, push, turn, push.
    m.turnSel(1); // row 2 is skipped while disabled → ACTIVE
    m.push();
    m.turnSel(1);
    m.push();
    CHECK(m.get("afs.active") == 1.0f);
    // DRIVE: a numeric row stepped by its ParamSpec.
    m.turnSel(2);
    m.push();
    m.turnSel(5);
    m.push();
    CHECK(m.get("afs.drive") > 0.0f);
    // PROCESSOR › EXTERNAL opens the PROCESSOR page.
    m.turnSel(-1);
    m.push();
    m.turnSel(1);
    m.push();
    m.f.settle();
    CHECK(m.get("afs.processor") == 1.0f);
    CHECK(m.shell().current().id == "mixer.afs");
}

TEST_CASE("m5ui.keyboard_only.outputs: route a track to OUT3-4 and set the HP CUE with the UI-120 keys (W20)", "[m5]")
{
    M5Fixture m;
    m.jump(io::Control::ExtMixer, false, "mixer"); // ⌘1
    REQUIRE(m.shell().current().id == "mixer");
    m.shell().selectTrack(1);
    m.turnSel(-1); // ',' from strip 1: the rail's OUTPUT SETTING
    m.push();      // Return
    m.f.settle();
    REQUIRE(m.shell().current().id == "mixer.output");
    // ASSIGN MODE stays M + 6CH; strips: '.' to track 5, Ctrl+3 ↑ = OUT3-4.
    m.turnSel(4);
    CHECK(m.state().selectedTrack == 5);
    m.knob(3, 1);
    CHECK(m.get("routing.outAssign", 5) == 2);
    // HP CUE: ',' back to the rail, Return, '.' to OUT3-4, Return.
    m.turnSel(-5);
    REQUIRE(m.shell().railFocus() == 3);
    m.push();
    m.turnSel(1);
    m.push();
    CHECK(m.get("routing.headphoneCue") == 2);
    m.click(io::Control::Back); // Esc → MIXER
    m.f.settle();
    CHECK(m.shell().current().id == "mixer");
    CHECK(m.hasText("3-4")); // UID-559: MIXER shows the same assignment
}
