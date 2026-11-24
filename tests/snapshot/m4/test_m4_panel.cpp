// Panel and keyboard reachability (NFR-040: every action reachable by panel or keyboard): the panel / keyboard paths
// of the performance features, through the real shell with the built-in registries —
//   SHIFT + sel.push moves the focus between a screen's body and its rail (our choice); turning past the rail's last
//   item returns it to the body on PLAYBACK, SET SLICE and AMP ENVELOPE (the MIXER / SCENE MANAGER convention):
//   PLAYBACK's SET SCALE / REVERSE / GRID SNAP, SET SLICE's ADD / DELETE / GRID SLICE and its zoom, AMP ENVELOPE's
//   SYNC; SHIFT + sel.push on HOME = BROWSE (UI-403); the jump keys ext.mixer / ext.browse / ext.utility / ext.project
//   and their SHIFT neighbours ARRANGER / TRACK SETTING (our choice); SHIFT + ext.tap = BPM/QUANTIZE, SHIFT + push
//   there = metronome (our choice); UTILITY › TOUCH STRIP SETTING (PITCH RANGE) (utility.md); SCENE MANAGER's COPY /
//   PASTE / DELETE on ext.copy / ext.paste / ext.clear; PROJECT › SAVE by the selector; the document keys (PanelKeys:
//   SHIFT + SCENE, SHIFT + lit SCALE, undo / redo, clear the held step) and the pad-mode lamps (PadLamps).
#include "core/commands/CommandBusAccess.h"
#include "core/commands/slice/SliceCommands.h"
#include "core/model/Project.h"
#include "core/util/playback/Grid.h"
#include "tests/fakes/FakeEngineBridge.h"
#include "tests/snapshot/m1/DemoSample.h"
#include "tests/snapshot/m2/M2Fixture.h"
#include "tests/snapshot/m4/M4Fixture.h"
#include "ui/input/pads/PadLamps.h"
#include "ui/input/panel/PanelKeys.h"
#include "ui/input/steps/StepController.h"
#include "ui/screens/sequence/M2Commands.h"
#include "ui/screens/setslice/SliceSync.h"
#include "ui/screens/touchstrip/PitchRange.h"
#include "ui/widgets/ListPopup.h"
#include "ui/widgets/Message.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>

using m4test::M4Fixture;
namespace cs = core::slice;
namespace sc = core::commands::slice;

namespace
{
bool logged(const std::vector<std::string>& log, std::string_view text)
{
    return std::any_of(log.begin(), log.end(), [&](const std::string& l) { return l.find(text) != std::string::npos; });
}

void shiftPush(m1test::M1Fixture& f)
{
    f.press(io::Control::Shift);
    f.press(io::Control::SelectorPush);
    f.release(io::Control::SelectorPush);
    f.release(io::Control::Shift);
    f.settle();
}

void shiftKey(m1test::M1Fixture& f, io::Control c)
{
    f.press(io::Control::Shift);
    f.press(c);
    f.release(c);
    f.release(io::Control::Shift);
    f.settle();
}

void push(m1test::M1Fixture& f)
{
    f.press(io::Control::SelectorPush);
    f.release(io::Control::SelectorPush);
    f.settle();
}

void turn(m1test::M1Fixture& f, int d)
{
    f.turn(io::Control::Selector, d);
    f.settle();
}
} // namespace

TEST_CASE("m4panel.playback: SHIFT + push gives the rail the focus; SET SCALE, REVERSE, GRID SNAP by the selector",
          "[m4][panel]")
{
    M4Fixture m;
    auto& f = m.f;
    REQUIRE(m.open("track.menu"));
    REQUIRE(m.open("track.playback"));
    CHECK(m.shell().railFocus() == -1); // the body (the waveform zoom) has the rotary
    shiftPush(f);
    CHECK(m.shell().railFocus() == 0); // SET SLICE
    CHECK(logged(f.log, "INPUT rail focus on"));
    turn(f, 2); // REVERSE
    REQUIRE(m.shell().railFocus() == 2);
    const float reverse = m.get("playback.reverse", 1);
    push(f);
    CHECK(m.get("playback.reverse", 1) != reverse);
    push(f);
    CHECK(m.get("playback.reverse", 1) == reverse);
    turn(f, 1); // GRID SNAP
    const float snap = m.get("playback.gridSnap", 1);
    push(f);
    CHECK(m.get("playback.gridSnap", 1) != snap);
    push(f);
    CHECK(logged(f.log, "INPUT rail GRID SNAP"));
    turn(f, -2); // SET SCALE
    push(f);
    CHECK(m.shell().current().id == "track.playback.setScale");
    f.press(io::Control::Back);
    f.settle();
    REQUIRE(m.shell().current().id == "track.playback");
    CHECK(m.shell().railFocus() == 1); // the remembered focus (UI-009)
    turn(f, 5); // past GRID SNAP: the body gets the rotary back
    CHECK(m.shell().railFocus() == -1);
    turn(f, 2); // zoom: the focus stays on the body
    CHECK(m.shell().railFocus() == -1);
    shiftPush(f);
    CHECK(m.shell().railFocus() == 0);
    shiftPush(f); // SHIFT + push again: back to the body
    CHECK(m.shell().railFocus() == -1);
    CHECK(logged(f.log, "INPUT rail focus off"));
}

TEST_CASE("m4panel.amp: AMP ENVELOPE's SYNC toggle is reachable by SHIFT + push and the selector", "[m4][panel]")
{
    M4Fixture m;
    auto& f = m.f;
    REQUIRE(m.open("track.menu"));
    REQUIRE(m.open("track.ampEnvelope"));
    shiftPush(f);
    CHECK(m.shell().railFocus() == 1); // SETTING (slot 1 is the PRESET hole)
    turn(f, 1);
    REQUIRE(m.shell().railFocus() == 2); // SYNC
    const float sync = m.get("env.sync", 1);
    push(f);
    CHECK(m.get("env.sync", 1) != sync);
    CHECK(logged(f.log, "INPUT rail SYNC"));
    push(f);
    CHECK(m.get("env.sync", 1) == sync);
    turn(f, 1); // past SYNC: the handles again
    CHECK(m.shell().railFocus() == -1);
    push(f); // push on the body = SETTING (unchanged)
    CHECK(m.shell().popupDepth() == 1);
}

TEST_CASE("m4panel.set_slice: ADD / DELETE / GRID SLICE by the selector; past the rail the rotary zooms",
          "[m4][panel]")
{
    snapshot::Session session;
    M4Fixture m;
    m1test::DemoSample beat{"beat"};
    ui::m1::SampleViews::instance().setProvider(
        [&beat](int t) { return t == 5 ? beat.view("Beat_120bpm_2bar") : ui::m1::SampleView{}; });
    ui::slice::sliceLive() = ui::slice::SliceLive{};
    m.state().selectedTrack = 5;
    REQUIRE(m.open("track.menu"));
    REQUIRE(m.open("track.playback"));
    REQUIRE(m.open("track.playback.setSlice"));
    auto& f = m.f;
    auto points = [] {
        return cs::slicePoints(sc::settings(ui::m2::Session::instance().project(), 0, 4), 176400).count;
    };
    REQUIRE(m.shell().railFocus() == 0); // ADD SLICE holds the focus on entry
    push(f);
    CHECK(points() == 17);
    turn(f, 1); // DELETE SLICE (the new slice is selected)
    push(f);
    CHECK(points() == 16);
    turn(f, 1); // GRID SLICE
    push(f);
    REQUIRE(m.shell().popupDepth() == 1);
    CHECK(dynamic_cast<const ui::ListPopup*>(m.shell().topPopup()) != nullptr);
    f.press(io::Control::Back);
    f.settle();
    turn(f, 1); // past GRID SLICE: the waveform zoom
    CHECK(m.shell().railFocus() == -1);
    turn(f, 3);
    CHECK(m.shell().railFocus() == -1);
    shiftPush(f);
    CHECK(m.shell().railFocus() == 0);
    ui::m1::SampleViews::instance().setProvider({});
    ui::slice::sliceLive() = ui::slice::SliceLive{};
}

TEST_CASE("m4panel.home: SHIFT + push on HOME opens BROWSE of the selected track (UI-403)", "[m4][panel]")
{
    M4Fixture m;
    auto& f = m.f;
    REQUIRE(m.shell().current().id == "home");
    shiftPush(f);
    CHECK(m.shell().current().id == "track.browse");
    f.press(io::Control::Home);
    f.settle();
    push(f); // a plain push is still TRACK MENU (UI-401)
    CHECK(m.shell().current().id == "track.menu");
}

TEST_CASE("m4panel.jump: ext.mixer / ext.browse / ext.utility / ext.project and SHIFT for ARRANGER / TRACK SETTING",
          "[m4][panel]")
{
    M4Fixture m;
    auto& f = m.f;
    auto key = [&](io::Control c)
    {
        f.press(c);
        f.release(c);
        f.settle();
        return std::string(m.shell().current().id);
    };
    CHECK(key(io::Control::ExtMixer) == "mixer");
    CHECK(key(io::Control::ExtUtility) == "utility");
    CHECK(key(io::Control::ExtProject) == "project");
    CHECK(key(io::Control::ExtBrowse) == "track.browse");
    shiftKey(f, io::Control::ExtMixer);
    CHECK(m.shell().current().id == "arranger");
    shiftKey(f, io::Control::ExtUtility);
    CHECK(m.shell().current().id == "trackSetting");
    CHECK(logged(f.log, "INPUT key shift+ext.utility -> trackSetting"));
    // A jump closes the popups on its way (as HOME does).
    REQUIRE(m.open("track.menu"));
    REQUIRE(m.shell().open("bpmQuantize"));
    REQUIRE(m.shell().popupDepth() == 1);
    CHECK(key(io::Control::ExtMixer) == "mixer");
    CHECK(m.shell().popupDepth() == 0);
}

TEST_CASE("m4panel.bpm: SHIFT + ext.tap opens BPM/QUANTIZE; the selector edits it; SHIFT + push = metronome",
          "[m4][panel]")
{
    M4Fixture m;
    auto& f = m.f;
    shiftKey(f, io::Control::ExtTap);
    REQUIRE(m.shell().popupDepth() == 1);
    CHECK(logged(f.log, "INPUT key shift+ext.tap -> bpmQuantize"));
    const auto* tempo = core::builtinParams().find("transport.tempo");
    const auto* metronome = core::builtinParams().find("transport.metronome");
    const float bpm = m.shell().values().get(*tempo, 0);
    turn(f, 2);
    CHECK(m.shell().values().get(*tempo, 0) == bpm + 2);
    CHECK(logged(f.log, "INPUT sel +2 (popup)"));
    const float metro = m.shell().values().get(*metronome, 0);
    shiftPush(f);
    CHECK(m.shell().values().get(*metronome, 0) != metro);
    shiftPush(f);
    CHECK(m.shell().values().get(*metronome, 0) == metro);
    shiftKey(f, io::Control::ExtTap); // open already: nothing else opens, nothing taps
    CHECK(m.shell().popupDepth() == 1);
    f.press(io::Control::Back);
    f.settle();
    CHECK(m.shell().popupDepth() == 0);
}

TEST_CASE("m4panel.pitch_range: UTILITY › TOUCH STRIP SETTING (PITCH RANGE) by the selector (utility.md, RIB-002)",
          "[m4][panel]")
{
    M4Fixture m;
    auto& f = m.f;
    auto& session = ui::m2::Session::instance();
    shiftKey(f, io::Control::Home); // UTILITY
    REQUIRE(m.shell().current().id == "utility");
    turn(f, -20);
    turn(f, 2); // USER1, USER2, PITCH RANGE
    char text[32];
    ui::m4::pitchRangeText(session.project().ribbon, text, sizeof text);
    CHECK(m.hasText(text)); // the row shows the value
    push(f);
    REQUIRE(m.shell().popupDepth() == 1);
    const auto* list = dynamic_cast<const ui::ListPopup*>(m.shell().topPopup());
    REQUIRE(list != nullptr);
    CHECK(list->focusedItem() == ui::m4::pitchRangeIndex(session.project().ribbon)); // the current value ✓
    turn(f, -20); // 1
    turn(f, 2);   // 3
    push(f);
    CHECK(m.shell().popupDepth() == 0);
    CHECK(session.project().ribbon.pitchTopCents == 300);
    CHECK(session.project().ribbon.pitchBottomCents == -300);
    f.settle();
    CHECK(m.hasText("3 Bipolar Semitones"));
    CHECK(session.undo()); // one undo step
    CHECK(session.project().ribbon.pitchTopCents != 300);
    core::model::RibbonSettings odd;
    odd.pitchBottomCents = -200;
    odd.pitchTopCents = 700;
    ui::m4::pitchRangeText(odd, text, sizeof text);
    CHECK(std::string_view(text) == "\xE2\x88\x92" "200 / +700 cents");
    CHECK(ui::m4::pitchRangeIndex(odd) == -1);
}

TEST_CASE("m4panel.scene_manager: COPY / PASTE / DELETE on ext.copy / ext.paste / ext.clear", "[m2][panel]")
{
    m2test::M2Fixture f;
    auto& s = f.session();
    s.execute(ui::m2::cmd::toggleStep(s.project(), 0, 0, 2, 5, core::model::TriggerType::Half, 127));
    REQUIRE(f.open("sceneManager"));
    f.press(io::Control::SelectorPush); // into the patterns of scene 1 (pattern 1 focused)
    f.settle();
    f.press(io::Control::ExtCopy);
    f.release(io::Control::ExtCopy);
    f.settle();
    CHECK(m2test::hasText(f.dl, "Pattern 1 copied"));
    CHECK(logged(f.log, "TOAST Pattern 1 copied"));
    f.turn(io::Control::Selector, 8); // pattern 9
    f.press(io::Control::ExtPaste);
    f.release(io::Control::ExtPaste);
    f.settle();
    CHECK(s.cell(0, 8, 2, 5).kind == ui::StepKind::Half);
    f.press(io::Control::ExtClear); // DELETE: pattern 9 is not playing, at once
    f.release(io::Control::ExtClear);
    f.settle();
    CHECK(s.cell(0, 8, 2, 5).kind == ui::StepKind::Empty);
    CHECK(logged(f.log, "TOAST Pattern 9 deleted"));
}

TEST_CASE("m4panel.project: PROJECT › SAVE by the selector and push (the name dialog, then OVERWRITE?)", "[m2][panel]")
{
    m2test::M2Fixture f;
    f.press(io::Control::ExtProject);
    f.settle();
    REQUIRE(f.shell->current().id == "project");
    f.turn(io::Control::Selector, -20);
    f.turn(io::Control::Selector, 2); // SAVE
    f.press(io::Control::SelectorPush);
    f.settle();
    REQUIRE(f.shell->popupDepth() == 1); // the first save asks for a name
    f.press(io::Control::SelectorPush);   // the dialog's primary: SAVE
    f.settle(4);
    CHECK(m2test::FakeProjectSource::instance().hasFile());
    CHECK(logged(f.log, "TOAST Saved"));
    f.press(io::Control::SelectorPush); // SAVE again: OVERWRITE? (focused: OVERWRITE)
    f.settle();
    REQUIRE(f.shell->popupDepth() == 1);
    f.press(io::Control::SelectorPush);
    f.settle(4);
    CHECK(std::count_if(f.log.begin(), f.log.end(), [](const std::string& l) { return l == "TOAST Saved"; }) == 2);
}

// --- the document keys (PanelKeys) and the pad-mode lamps (PadLamps) -----------------------------------------------
namespace
{
struct Document
{
    M4Fixture m;
    core::CommandBus bus;
    fakes::FakeEngineBridge bridge;
    ui::input::pads::SimpleSelection selection;
    std::unique_ptr<ui::input::pads::PadController> pads;
    std::unique_ptr<ui::input::steps::StepController> steps;
    std::unique_ptr<ui::input::panel::PanelKeys> keys;
    std::vector<std::string> log;
    Document()
    {
        auto project = std::make_unique<core::model::Project>();
        for (auto& t : project->scenes[0].tracks)
            t.playback.sample.id = "kit"; // every track has content (pad lamps light, UI-151)
        core::commands::replaceProject(bus, std::move(project));
        pads = std::make_unique<ui::input::pads::PadController>(bus, bridge, selection);
        steps = std::make_unique<ui::input::steps::StepController>(bus, &bridge);
        keys = std::make_unique<ui::input::panel::PanelKeys>(bus, *pads, *steps);
        keys->attach(&m.shell());
        keys->log = [this](const std::string& l) { log.push_back(l); };
    }
    bool press(io::Control c, bool shift = false)
    {
        return keys->handle({io::ControlEvent::Kind::Press, c, 100, io::kSourceKeyboard, m.f.now}, shift);
    }
    const core::model::Step& cell(int step0, int track0)
    {
        return core::commands::project(bus).scenes[0].patterns[0].steps[static_cast<std::size_t>(step0)]
                                                                          [static_cast<std::size_t>(track0)];
    }
};
} // namespace

TEST_CASE("m4panel.keys: SHIFT + SCENE opens / closes SCENE MANAGER; SHIFT + lit SCALE opens SET SCALE (UI-403)",
          "[m4][panel]")
{
    Document d;
    CHECK_FALSE(d.press(io::Control::Scene)); // a plain SCENE is the SCENE mode's
    CHECK(d.press(io::Control::Scene, true));
    d.m.f.settle();
    CHECK(d.m.shell().current().id == "sceneManager");
    CHECK(d.press(io::Control::Scene, true));
    d.m.f.settle();
    CHECK(d.m.shell().current().id == "home");
    CHECK_FALSE(d.press(io::Control::PadModeScale, true)); // SCALE not lit: the pad controller's
    d.pads->onModeKey(ui::input::pads::PadMode::Scale);
    CHECK(d.press(io::Control::PadModeScale, true));
    d.m.f.settle();
    CHECK(d.m.shell().current().id == "track.playback.setScale");
    CHECK(d.pads->mode() == ui::input::pads::PadMode::Scale); // the mode stays on
    d.m.f.press(io::Control::Back);
    d.m.f.settle();
    CHECK(d.m.shell().current().id == "track.playback"); // BACK goes one level up
}

TEST_CASE("m4panel.keys: SHIFT + REC opens LIVE SAMPLING from anywhere; BACK returns to TRACK MENU (W3)", "[m4][panel]")
{
    Document d;
    CHECK_FALSE(d.press(io::Control::Rec)); // a plain REC is the transport's
    CHECK(d.press(io::Control::Rec, true));
    d.m.f.settle();
    CHECK(d.m.shell().current().id == "sampling.live");
    CHECK(logged(d.log, "INPUT key shift+rec -> sampling.live"));
    CHECK_FALSE(d.press(io::Control::Rec, true)); // on LIVE SAMPLING the key is the screen's REC
    d.m.f.press(io::Control::Back);
    d.m.f.settle();
    CHECK(d.m.shell().current().id == "track.menu");
    REQUIRE(d.m.open("sampling.edit"));
    CHECK_FALSE(d.press(io::Control::Rec, true)); // SAMPLING/EDIT keeps its REC too
    CHECK(d.m.shell().current().id == "sampling.edit");
}

TEST_CASE("m4panel.keys: ext.undo / ext.redo; ext.clear clears the held step and its release toggles nothing",
          "[m4][panel]")
{
    Document d;
    d.steps->setContext(0, 0, 2);
    d.steps->onStepKey(5, true, 0);
    d.steps->onStepKey(5, false, 10); // a trigger on step 5, track 3
    REQUIRE(d.cell(4, 2).trigger != core::model::Step::kEmpty);
    CHECK(d.press(io::Control::ExtUndo));
    CHECK(d.cell(4, 2).trigger == core::model::Step::kEmpty);
    CHECK(d.press(io::Control::ExtRedo));
    CHECK(d.cell(4, 2).trigger != core::model::Step::kEmpty);
    CHECK(logged(d.log, "EDIT undo ok"));
    CHECK(logged(d.log, "EDIT redo ok"));
    CHECK_FALSE(d.press(io::Control::ExtClear)); // no step held: the screen's (SCENE MANAGER: DELETE)
    d.steps->onStepKey(5, true, 20);
    CHECK(d.press(io::Control::ExtClear));
    d.steps->onStepKey(5, false, 30);
    CHECK(d.cell(4, 2).trigger == core::model::Step::kEmpty); // cleared, and the release did not toggle it back
    CHECK(logged(d.log, "EDIT clear step=5 track=3 ok"));
    // A recording pass is one undo step: undo waits until it ends.
    core::commands::beginGroup(d.bus);
    CHECK(d.press(io::Control::ExtUndo));
    CHECK(logged(d.log, "EDIT undo refused"));
    core::commands::endGroup(d.bus);
}

TEST_CASE("m4panel.lamps: the pad controller paints the pads in every pad mode over the shell's frame, without flicker",
          "[m4][panel]")
{
    Document d;
    using ui::input::pads::PadMode;
    ui::input::pads::PadLamps lamps;
    io::LampFrame base{};
    for (int p = 1; p <= 16; ++p)
        base[io::pad(p)] = {io::LampMode::On, 0x123456, 0}; // the shell's pads
    auto shown = [&](double now)
    {
        io::LampFrame f = base;
        d.pads->paintLamps(lamps, now, d.bridge.meters());
        lamps.overlay(f);
        return f;
    };
    auto level = [](const io::LampFrame& f, int pad) { return io::resolve(f[io::pad(pad)], 0).level; };
    // PLAY: the controller's pads (the same ladder as every mode): track 1 selected full, the others dim; mode keys
    // off.
    auto f = shown(0);
    CHECK(f[io::pad(1)].rgb != 0x123456);
    CHECK(level(f, 1) == 255);
    CHECK(level(f, 2) == io::kDimLevel);
    CHECK(f[io::Control::PadModeMute].mode == io::LampMode::Off);
    // A sequencer step on track 2 (the engine's hit count moves): its pad flashes white, then eases back to dim.
    engine::PlayheadFeed ph{};
    d.pads->observeHits(ph, 50); // the first call only takes the counts
    ph.trackHits[1] = 1;
    d.pads->observeHits(ph, 100);
    f = shown(100);
    CHECK((level(f, 2) == 255 && f[io::pad(2)].rgb == io::lampcolour::white));
    CHECK(level(f, 3) == io::kDimLevel); // the other pads stay
    f = shown(1200);
    CHECK(level(f, 2) == io::kDimLevel);
    // MUTE: the mode key white, a muted track's pad at the minimum, the others as in PLAY.
    d.pads->onModeKey(PadMode::Mute);
    d.pads->onPad(2, 100, false, true, 1300, true); // mute track 3
    f = shown(2000);
    CHECK(f[io::Control::PadModeMute].mode == io::LampMode::On);
    CHECK(f[io::Control::PadModeTrack].mode == io::LampMode::Off);
    CHECK(f[io::pad(3)].mode == io::LampMode::Faint); // a muted pad: just a small light
    CHECK(level(f, 1) == 255);
    CHECK(level(f, 2) == io::kDimLevel);
    // A sequencer hit on the muted track does not flash it (it does not sound).
    ph.trackHits[2] = 1;
    d.pads->observeHits(ph, 2100);
    f = shown(2100);
    CHECK(f[io::pad(3)].mode == io::LampMode::Faint);
    // A frame inside the 60 Hz throttle (no new paint) still shows the last paint, not the shell's pads.
    f = shown(2105);
    CHECK(f[io::pad(3)].mode == io::LampMode::Faint);
    CHECK(f[io::Control::PadModeMute].mode == io::LampMode::On);
    // SCALE: roots full, the other notes dim.
    d.pads->onModeKey(PadMode::Scale);
    f = shown(3000);
    CHECK(f[io::Control::PadModeScale].mode == io::LampMode::On);
    int on = 0, dim = 0;
    for (int p = 1; p <= 16; ++p)
        (level(f, p) == 255 ? on : dim) += level(f, p) == 0 ? 0 : 1;
    CHECK(on > 0);
    CHECK(dim > 0);
    // TRACK: the same ladder as PLAY.
    d.pads->onModeKey(PadMode::Track);
    f = shown(4000);
    CHECK(f[io::Control::PadModeTrack].mode == io::LampMode::On);
    CHECK(level(f, 1) == 255); // track 1 selected
    CHECK(level(f, 2) == io::kDimLevel);
    // SLICE: the selected track's slice pads in its colour, dim until hit.
    d.pads->onModeKey(PadMode::Slice);
    f = shown(5000);
    CHECK(f[io::Control::PadModeSlice].mode == io::LampMode::On);
    CHECK(level(f, 16) == io::kDimLevel);
    // Back to PLAY: every mode key off, the pads still the controller's.
    d.pads->onModeKey(PadMode::Slice);
    f = shown(6000);
    CHECK(f[io::pad(1)].rgb != 0x123456);
    CHECK(f[io::Control::PadModeSlice].mode == io::LampMode::Off);
}
