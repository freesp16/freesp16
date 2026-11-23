// SAMPLING/EDIT and LIVE SAMPLING (sampling.md UID-775…789, screens.md UI-320…324): the three mockups —
// sampling-edit.svg (track 5 with a sample, stopped), sampling-recording.svg (2 bars into track 2, 56 % done) and
// live-sampling.svg (track 5 armed at the next bar, the last take nudged) — with generated ring contents shaped like
// the mockups' pseudo waveforms (kit_track.py amplitudes()); REC / standby / recording states, columns 1–5 locked while
// recording, the EDIT list (BAR ⇄ BPM), SAVE SELECTION, the unsaved-take guard, the live REC / NUDGE / SAVE / UNLOAD
// actions, and the keyboard-only workflows W2 / W3 (UI-420).
#include "tests/snapshot/m1/DemoSample.h"
#include "tests/snapshot/m5/FakeSamplingHost.h"
#include "tests/snapshot/m5/M5Fixture.h"
#include "ui/shell/Layout.h"
#include "ui/widgets/Dialog.h"
#include "ui/widgets/RingView.h"
#include "ui/widgets/SettingPopup.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace snapshot;
using m5test::FakeSamplingHost;
using m5test::M5Fixture;
namespace L = ui::layout;
using core::sampling::LiveState;
using core::sampling::RecState;

namespace ui
{
int samplingFilledColumns(const Widget* body) noexcept; // ui/screens/sampling/SamplingScreen.cpp
const RingView* liveRingOf(const Widget* body) noexcept; // ui/screens/livesampling/LiveSamplingScreen.cpp
} // namespace ui

namespace
{
float lin(float db)
{
    return std::pow(10.0f, db / 20.0f);
}
// The linear peak whose meter bar reaches `fraction` of the 184–600 column (the mockups' meter(level=…)).
float peakAt(float fraction)
{
    const float y = 600 - 416 * fraction;
    return lin(3.0f - (y - 184) * 53.0f / 404.0f);
}

struct SamplingFixture : M5Fixture
{
    FakeSamplingHost host;
    m1test::DemoSample beat {"beat", 176400, {280, 404, 70}};
    bool withSample = true;
    SamplingFixture()
    {
        ui::m1::SampleViews::instance().setProvider(
            [this](int t) { return t == 5 && withSample ? beat.view("Beat_120bpm_2bar") : ui::m1::SampleView {}; });
        auto& st = state();
        st.selectedTrack = 5;
        auto& t5 = st.tracks[4];
        t5.typeBadge = "SMPL";
        t5.stretchBadge = "M.TMP";
        t5.loopGlyph = "\xE2\x9F\xB2";
        t5.name = "Beat_120bpm_2bar";
        t5.hasContent = true;
        set("playback.length", 5, 176400);
    }
    ~SamplingFixture() { ui::m1::SampleViews::instance().setProvider({}); }
    void levels(float l, float r)
    {
        host.hubRef().status.peakL = peakAt(l);
        host.hubRef().status.peakR = peakAt(r);
    }
};
} // namespace

TEST_CASE("snapshot.m5.sampling_edit: SAMPLING/EDIT (track 5 with a sample, stopped) matches sampling-edit.svg",
          "[snapshot][m5]")
{
    Session session;
    SamplingFixture m;
    REQUIRE(m.open("track.menu"));
    REQUIRE(m.open("sampling.edit"));
    m.turnSel(1); // REC holds the focus
    m.levels(0.18f, 0.16f);
    const auto ours = m.f.image();
    CHECK(m.shell().current().parent == "track.menu");
    CHECK(m.shell().railFocus() == 0);
    CHECK(m.state().sceneLocked); // UID-776
    CHECK(m.hasText("176400"));   // SELECTION END = the whole take
    CHECK(m.shell().column(3).spec->id == "sampling.selStart");
    CHECK(m.get("sampling.screenOpen") == 1.0f);
    const auto& mock = m5test::m5mockup("sampling-edit");
    CHECK(compare(ours, mock, {16, 152, 192, 456}, "m5-sampling-rail").match >= kGate);
    // The INPUT column departs from the mockup by request (bars centred, as on every meter column): artifacts only.
    compare(ours, mock, {224, 152, 88, 456}, "m5-sampling-meter");
    CHECK(compare(ours, mock, {328, 152, 856, 360}, "m5-sampling-editor").match >= kGate);
    CHECK(compare(ours, mock, {328, 528, 856, 64}, "m5-sampling-overview").match >= kGate);
    CHECK(compare(ours, mock, L::contextBar, "m5-sampling-context").match >= kGate);
    CHECK(compare(ours, mock, L::knobStrip, "m5-sampling-strip").match >= kGate);
    CHECK(compare(ours, mock, L::canvas, "m5-sampling-edit").match >= kGate);
}

TEST_CASE("snapshot.m5.sampling_recording: the take grows (2 bars, 56 %) as in sampling-recording.svg",
          "[snapshot][m5]")
{
    Session session;
    SamplingFixture m;
    m.withSample = false;
    m.state().selectedTrack = 2;
    m.state().tracks[1].name = "RECORDED TEMP.";
    m.state().tracks[1].hasContent = true;
    REQUIRE(m.open("sampling.edit"));
    m.set("sampling.length", 2, 2); // REC LENGTH 2 BARS
    // The take: 2 bars at 120 BPM = 176400 frames over 280 columns (630 frames each); 56 % recorded.
    auto& hub = m.host.hubRef();
    m.host.writeSilence(1000);
    const auto start = hub.ring.head();
    const auto amps = m1test::amplitudes(156, "snare", 4);
    m.host.writeColumns(156, 630.0, [&](int c) { return amps[static_cast<std::size_t>(c)]; });
    m.host.writeSilence(98784 - 156 * 630);
    auto& s = hub.status;
    s.takeScene = 0;
    s.takeTrack = 1;
    s.takeSeq = 7;
    s.takeBars = 2;
    s.takeBpm = 120.0f;
    s.takeStart = start;
    s.takeEnd = -1;
    s.state = static_cast<int>(RecState::Recording);
    m.levels(0.86f, 0.9f);
    m.state().position.set("001:02:05");
    const auto ours = m.f.image();
    CHECK(ui::samplingFilledColumns(m.shell().body()) == 156);
    for (int c = 0; c < 5; ++c) // UI-320 / UID-778: columns 1–5 locked, MONITOR LEVEL editable
        CHECK(m.shell().column(c).disabled);
    CHECK_FALSE(m.shell().column(5).disabled);
    CHECK(m.state().samplerRec); // UID-777: the status row's REC mirrors the sampler
    CHECK(m.hasText("END \xC2\xB7 REC LENGTH 2 BARS"));
    const auto& mock = m5test::m5mockup("sampling-recording");
    CHECK(compare(ours, mock, {328, 152, 856, 360}, "m5-recording-editor").match >= kGate);
    CHECK(compare(ours, mock, {16, 152, 192, 456}, "m5-recording-rail").match >= kGate);
    CHECK(compare(ours, mock, L::knobStrip, "m5-recording-strip").match >= kGate);
    CHECK(compare(ours, mock, L::canvas, "m5-sampling-recording").match >= kGate);
    // REC (the rail item or the REC key) stops it.
    m.f.press(io::Control::Rec);
    CHECK(m.host.called("stop 0 1"));
}

TEST_CASE("snapshot.m5.live_sampling: LIVE SAMPLING (armed at the next bar, the last take nudged) matches "
          "live-sampling.svg",
          "[snapshot][m5]")
{
    Session session;
    SamplingFixture m;
    REQUIRE(m.open("sampling.live"));
    m.set("live.length", 1, 3); // 2 BARS
    m.set("live.loopAfterRec", 1, 1);
    m.set("live.nudge", 1, 529); // +12 ms
    // 210 past columns of 2520 frames (8 bars over 280 columns at 120 BPM) from ring frame 0: NOW on a bar line.
    auto& hub = m.host.hubRef();
    const auto amps = m1test::amplitudes(210, "beat", 7);
    m.host.writeColumns(210, 2520.0, [&](int c) { return amps[static_cast<std::size_t>(c)]; });
    auto& s = hub.status;
    const auto now = hub.ring.head();
    s.liveState = static_cast<int>(LiveState::Armed);
    s.liveStart = now - 6 * 88200; // the last take: bars −6 … −4 of the strip
    s.liveLength = 2 * 88200;
    s.liveNudge = 529;
    m.host.liveTake = 4;
    m.host.liveDirty = false;
    m.levels(0.5f, 0.46f);
    m.play(24.0); // a bar start
    m.state().position.set("003:04:13");
    const auto ours = m.f.image();
    const auto* ring = ui::liveRingOf(m.shell().body());
    REQUIRE(ring != nullptr);
    CHECK(ring->nowFrame() == now);
    CHECK(std::fabs(ring->peakAt(now - 2520) - amps[209]) < 1e-6f);
    CHECK(m.hasText("NOW"));
    CHECK(m.hasText("2 BARS"));
    CHECK(m.shell().column(3).valueText == "+12 ms");
    const auto& mock = m5test::m5mockup("live-sampling");
    CHECK(compare(ours, mock, {328, 152, 856, 360}, "m5-live-ring").match >= kGate);
    CHECK(compare(ours, mock, {328, 528, 856, 64}, "m5-live-take").match >= kGate);
    CHECK(compare(ours, mock, {16, 152, 192, 456}, "m5-live-rail").match >= kGate);
    // The INPUT column departs from the mockup by request (bars centred, as on every meter column): artifacts only.
    compare(ours, mock, {224, 152, 88, 456}, "m5-live-meter");
    CHECK(compare(ours, mock, L::knobStrip, "m5-live-strip").match >= kGate);
    CHECK(compare(ours, mock, L::canvas, "m5-live-sampling").match >= kGate);
}

TEST_CASE("m5ui.sampling: REC states, EDIT list BAR ⇄ BPM, SAVE SELECTION, the unsaved-take guard", "[m5]")
{
    SamplingFixture m;
    REQUIRE(m.open("track.menu"));
    REQUIRE(m.open("sampling.edit"));
    // REC: standby, then recording; the REC key does the same as the rail item.
    m.turnSel(1);
    m.push();
    m.f.settle();
    CHECK(m.host.called("record 0 4"));
    CHECK(m.hasText("WAITING FOR THRESHOLD"));
    CHECK(m.shell().column(0).disabled);
    m.f.press(io::Control::Rec);
    CHECK(m.host.called("stop 0 4"));
    m.f.settle();
    CHECK_FALSE(m.shell().column(0).disabled);
    // SELECTION START / END edit the take's region (one column per detent, a frame with SHIFT).
    m.knob(4, 2);
    CHECK(m.get("playback.start", 5) == 1260.0f); // 2 × 176400 / 280
    m.f.press(io::Control::Shift);
    m.knob(5, -3);
    m.f.release(io::Control::Shift);
    CHECK(m.get("playback.length", 5) == 176400.0f - 1260.0f - 3.0f);
    // EDIT list: BAR → BPM follows (2 bars in 4 s = 120 BPM; 4 bars = 240 BPM).
    m.turnSel(2);
    REQUIRE(m.shell().railFocus() == 2);
    m.push();
    m.f.settle();
    auto* edit = dynamic_cast<ui::SettingPopup*>(m.shell().topPopup());
    REQUIRE(edit != nullptr);
    CHECK(edit->row(0).label == "BAR");
    CHECK(edit->row(1).label == "BPM");
    CHECK(edit->row(5).label == "FADE OUT");
    m.push();         // edit BAR
    m.turnSel(2);     // 4
    m.push();         // confirm
    CHECK(m.host.tempoBars == 4.0);
    CHECK(std::fabs(m.host.tempoBpm - 240.0) < 1e-6);
    CHECK(std::fabs(edit->row(1).value - 240.0f) < 1e-3f);
    m.turnSel(4); // FADE IN (an action row: one push)
    m.push();
    m.f.settle();
    CHECK(m.host.called("process 4 1"));
    // SAVE CHANGES → SAVE SELECTION with the numbered name; SAVE & REPLACE (primary, Return).
    m.host.unsavedTrack = 4;
    m.turnSel(1);
    REQUIRE(m.shell().railFocus() == 3);
    m.push();
    m.f.settle();
    auto* dialog = dynamic_cast<ui::Dialog*>(m.shell().topPopup());
    REQUIRE(dialog != nullptr);
    CHECK(dialog->field().text() == "Beat_120bpm_2bar_01");
    m.shell().textKey(ui::TextKey::Return);
    m.f.settle();
    CHECK(m.host.called("save 4 1260 176397 Beat_120bpm_2bar_01 replace"));
    // Leaving with an unsaved take asks first; DISCARD leaves (UID-784).
    m.host.unsavedTrack = 4;
    m.click(io::Control::Back);
    m.f.settle();
    CHECK(m.shell().current().id == "sampling.edit");
    REQUIRE(m.shell().popupDepth() == 1);
    CHECK(m.hasText("Discard the recording?"));
    m.turnSel(1); // DISCARD
    m.push();
    m.f.settle();
    CHECK(m.host.called("discard 4"));
    CHECK(m.shell().current().id == "track.menu");
    CHECK(m.get("sampling.screenOpen") == 0.0f);
}

TEST_CASE("m5ui.live_sampling: REC arms / disarms, NUDGE moves the take, SAVE and UNLOAD", "[m5]")
{
    SamplingFixture m;
    m.state().selectedTrack = 7;
    REQUIRE(m.open("sampling.live"));
    CHECK(m.get("live.track") == 6.0f); // the selected track on entry
    CHECK(m.get("live.mode") == 1.0f);
    m.f.press(io::Control::Rec);
    CHECK(m.host.called("arm 0 6"));
    m.f.settle();
    CHECK(m.state().samplerRec); // armed blinks: on at the frame's whole second
    m.f.press(io::Control::Rec);
    CHECK(m.host.called("disarm 6"));
    m.knob(4, 2); // NUDGE: two 64-frame steps
    CHECK(m.host.nudge == 128);
    m.f.press(io::Control::Shift);
    m.knob(4, -1);
    m.f.release(io::Control::Shift);
    CHECK(m.host.nudge == 127);
    // No take: UNLOAD / SAVE disabled; with one they act.
    m.railTo(2);
    m.push();
    CHECK_FALSE(m.host.called("unload"));
    m.host.liveTake = 6;
    m.f.settle();
    CHECK(m.hasText("UNSAVED TAKE"));
    m.push();
    CHECK(m.host.called("unload 6"));
    m.host.liveTake = 6;
    m.railTo(3);
    m.push();
    CHECK(m.host.called("saveLive 0"));
    m.click(io::Control::Back);
    m.f.settle();
    CHECK(m.get("live.mode") == 0.0f);
}

TEST_CASE("m5ui.keyboard_only.sampling: record, trim and save a take with the UI-120 keys (W2, UI-420)", "[m5]")
{
    SamplingFixture m;
    m.withSample = false;
    // HOME (track 5) → Return → TRACK MENU (BROWSE focused) → '.' → SAMPLING/EDIT → Return.
    m.push();
    m.f.settle();
    REQUIRE(m.shell().current().id == "track.menu");
    m.turnSel(1);
    m.push();
    m.f.settle();
    REQUIRE(m.shell().current().id == "sampling.edit");
    m.key(io::Control::Rec); // G: standby
    CHECK(m.host.called("record 0 4"));
    m.key(io::Control::Rec); // G: stop
    CHECK(m.host.called("stop 0 4"));
    // The take arrives on the track (RECORDED TEMP.).
    m.withSample = true;
    m.host.unsavedTrack = 4;
    m.f.settle();
    m.knob(4, 1); // Ctrl+4 ↑: SELECTION START one column in
    CHECK(m.get("playback.start", 5) == 630.0f);
    m.railTo(3); // '.' to SAVE CHANGES
    REQUIRE(m.shell().railFocus() == 3);
    m.push();
    m.shell().textKey(ui::TextKey::Return); // SAVE & REPLACE
    m.f.settle();
    CHECK(m.host.called("save 4 630 176400 Beat_120bpm_2bar_01 replace"));
    m.click(io::Control::Back); // Esc: saved, nothing to ask
    m.f.settle();
    CHECK(m.shell().current().id == "track.menu");
}

TEST_CASE("m5ui.keyboard_only.live_sampling: arm a live take, nudge and save it with the UI-120 keys (W3, UI-420)",
          "[m5]")
{
    SamplingFixture m;
    m.push(); // HOME → TRACK MENU
    m.f.settle();
    m.turnSel(-1); // ',' → LIVE SAMPLING
    m.push();
    m.f.settle();
    REQUIRE(m.shell().current().id == "sampling.live");
    m.knob(2, 1); // Ctrl+2 ↑: LENGTH 2 BARS
    CHECK(m.get("live.length") == 3.0f);
    m.key(io::Control::Rec); // G: armed for the next bar
    CHECK(m.host.called("arm 0 4"));
    m.host.liveTake = 4; // the take is on the track
    m.knob(4, 3);        // Ctrl+4 ↑↑↑: NUDGE
    CHECK(m.host.nudge == 192);
    m.railTo(3); // '.' to SAVE
    REQUIRE(m.shell().railFocus() == 3);
    m.push();
    CHECK(m.host.called("saveLive 0"));
}
