// ui::input::pads::PadController and the track commands (PAD-001/003/004/006, TRK-002/041, UI-104/118/151/152):
// one-hot mode keys, selection, MUTE mode through the CommandBus (one undo step for SHIFT + pad), what reaches the
// engine bridge, lamps; core::commands::track forwarding to the engine.
#include "core/commands/CommandBusAccess.h"
#include "core/commands/UndoStack.h"
#include "core/commands/scene/SceneCommands.h"
#include "core/commands/track/TrackCommands.h"
#include "core/model/Project.h"
#include "core/params/ParamRegistry.h"
#include "io/controllers/LampRules.h"
#include "tests/fakes/FakeEngineBridge.h"
#include "ui/input/pads/PadController.h"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <memory>
#include <variant>

namespace
{
namespace cmd = core::commands;
using ui::input::pads::PadController;
using ui::input::pads::PadMode;

struct World
{
    core::CommandBus bus;
    fakes::FakeEngineBridge bridge;
    ui::input::pads::SimpleSelection selection;
    std::unique_ptr<PadController> pads;
    World()
    {
        cmd::replaceProject(bus, std::make_unique<core::model::Project>());
        // The composition root's bridge hook: model changes of the track commands → engine ParamChanges.
        cmd::setBridge(bus, [this](const core::ChangeSet& c, const core::model::Project& p) {
            ui::input::pads::forwardTrackChanges(c, p, bridge);
        });
        pads = std::make_unique<PadController>(bus, bridge, selection);
    }
    const core::model::Project& project() { return cmd::project(bus); }
    // The last value posted for a parameter (ParamChange) and the TriggerNows posted.
    float lastParam(const char* name, std::uint16_t scope = 0xFFFF)
    {
        const auto pid = core::builtinParams().idOf(name).value_or(core::ParamId {});
        float v = -999.0f;
        for (const auto& m : bridge.posted)
            if (const auto* c = std::get_if<engine::ParamChange>(&m); c != nullptr && c->param == pid && c->scope == scope)
                v = c->value;
        return v;
    }
    std::vector<engine::TriggerNow> triggers()
    {
        std::vector<engine::TriggerNow> out;
        for (const auto& m : bridge.posted)
            if (const auto* t = std::get_if<engine::TriggerNow>(&m))
                out.push_back(*t);
        return out;
    }
};

struct Lamps final : io::LampSink
{
    std::map<io::Control, std::pair<std::uint32_t, std::uint8_t>> lamps;
    int flushes = 0;
    void setLamp(io::Control c, std::uint32_t rgb, std::uint8_t level) override { lamps[c] = {rgb, level}; }
    void flush() override { ++flushes; }
};
} // namespace

TEST_CASE("trigger.pads: mode keys are one-hot, the lit key turns its mode off (PAD-001, UI-104)", "[trigger]")
{
    World w;
    auto& p = *w.pads;
    CHECK(p.mode() == PadMode::Play);
    const PadMode keys[] = {PadMode::Track, PadMode::Mute, PadMode::Slice, PadMode::Scale};
    for (const auto k : keys)
    {
        p.onModeKey(k);
        CHECK(p.mode() == k);
        CHECK(w.lastParam("pads.mode") == static_cast<float>(k));
    }
    p.onModeKey(PadMode::Mute); // another key: one-hot
    CHECK(p.mode() == PadMode::Mute);
    p.onModeKey(PadMode::Mute); // the lit key again: none lit
    CHECK(p.mode() == PadMode::Play);
    CHECK(w.lastParam("pads.mode") == 0.0f);
    // Through the event path (F5–F8 / panel keys).
    CHECK(p.handle({io::ControlEvent::Kind::Press, io::Control::PadModeScale, 0, io::kSourceKeyboard, 0.0}));
    CHECK(p.mode() == PadMode::Scale);
    CHECK_FALSE(p.handle({io::ControlEvent::Kind::Release, io::Control::PadModeScale, 0, io::kSourceKeyboard, 0.0}));
    CHECK(p.mode() == PadMode::Scale);
}

TEST_CASE("trigger.pads: SLICE mode cannot be used on a MIDI or AS-1 track", "[trigger]")
{
    World w;
    auto& p = *w.pads;
    w.selection.select(2);
    cmd::project(w.bus).scenes[0].tracks[2].mode = core::model::TrackMode::Midi;
    p.onModeKey(PadMode::Slice);
    CHECK(p.mode() == PadMode::Play); // refused: the key stays dark
    cmd::project(w.bus).scenes[0].tracks[2].mode = core::model::TrackMode::Instrument;
    p.onModeKey(PadMode::Slice);
    CHECK(p.mode() == PadMode::Play);
    p.onModeKey(PadMode::Scale); // SCALE is theirs (SET SCALE (MIDI))
    CHECK(p.mode() == PadMode::Scale);
    cmd::project(w.bus).scenes[0].tracks[2].mode = core::model::TrackMode::Sample;
    p.onModeKey(PadMode::Slice);
    CHECK(p.mode() == PadMode::Slice);
}

TEST_CASE("trigger.pads: selecting a MIDI or AS-1 track ends SLICE mode, as its key refuses it there", "[trigger]")
{
    World w;
    auto& p = *w.pads;
    cmd::project(w.bus).scenes[0].tracks[4].mode = core::model::TrackMode::Midi;
    cmd::project(w.bus).scenes[0].tracks[5].mode = core::model::TrackMode::Instrument;
    w.selection.select(2);
    p.onModeKey(PadMode::Slice);
    REQUIRE(p.mode() == PadMode::Slice);
    p.onPad(4, 100, true, true, 0, false); // SHIFT + pad 5 selects the MIDI track
    CHECK(w.selection.selected() == 4);
    CHECK(p.mode() == PadMode::Play);
    CHECK(w.lastParam("pads.mode") == static_cast<float>(PadMode::Play));
    CHECK(w.triggers().empty()); // no slice played meanwhile

    w.selection.select(2);
    p.onModeKey(PadMode::Slice);
    REQUIRE(p.mode() == PadMode::Slice);
    w.selection.select(5); // the rotary / HOME: the frame's sync sees an AS-1 track
    p.sync();
    CHECK(p.mode() == PadMode::Play);

    w.selection.select(2);
    p.onModeKey(PadMode::Slice);
    cmd::project(w.bus).scenes[0].tracks[2].mode = core::model::TrackMode::Midi; // the selected track made MIDI
    p.sync();
    CHECK(p.mode() == PadMode::Play);

    cmd::project(w.bus).scenes[0].tracks[2].mode = core::model::TrackMode::Sample;
    p.onModeKey(PadMode::Slice);
    p.onPad(3, 100, true, true, 0, false); // SHIFT + pad 4: a Sample track keeps SLICE mode
    CHECK(p.mode() == PadMode::Slice);
}

TEST_CASE("trigger.pads: PAD MODE SELECT RESPONSE OVER 1sec switches only after a 1 s hold (PAD-007)", "[trigger]")
{
    World w;
    auto& p = *w.pads;
    using Kind = io::ControlEvent::Kind;
    p.setModeSelectHoldMs(1000.0);
    CHECK(p.handle({Kind::Press, io::Control::PadModeMute, 0, io::kSourceKeyboard, 0.0}));
    p.tick(5000.0); // the first frame after the press starts the hold
    p.tick(5999.0);
    CHECK(p.mode() == PadMode::Play);
    CHECK(p.handle({Kind::Release, io::Control::PadModeMute, 0, io::kSourceKeyboard, 0.0})); // too short
    p.tick(7000.0);
    CHECK(p.mode() == PadMode::Play);
    p.handle({Kind::Press, io::Control::PadModeMute, 0, io::kSourceKeyboard, 0.0});
    p.tick(8000.0);
    p.tick(9000.0); // held 1 s: MUTE on while still held
    CHECK(p.mode() == PadMode::Mute);
    p.handle({Kind::Release, io::Control::PadModeMute, 0, io::kSourceKeyboard, 0.0});
    p.tick(9500.0);
    CHECK(p.mode() == PadMode::Mute);
    // IMMEDIATELY again: a press switches at once.
    p.setModeSelectHoldMs(0.0);
    p.handle({Kind::Press, io::Control::PadModeMute, 0, io::kSourceKeyboard, 0.0});
    CHECK(p.mode() == PadMode::Play);
}

TEST_CASE("trigger.pads: PLAY selects and plays pad n's track; TRACK selects silently; SHIFT + pad selects",
          "[trigger]")
{
    World w;
    auto& p = *w.pads;
    p.onPad(4, 100, false, true, 10.0); // pad 5, sent directly by the panel (UI-118)
    CHECK(w.selection.selected() == 4);
    CHECK(w.lastParam("pads.selected") == 4.0f);
    CHECK(w.triggers().empty()); // the UI never re-triggers a direct hit
    CHECK(p.lastHitMs(4) == 10.0);
    p.onPad(4, 0, false, false, 20.0); // release: always posted (GATE needs it)
    REQUIRE(w.triggers().size() == 1);
    CHECK(w.triggers()[0].velocity == 0);
    CHECK(w.triggers()[0].pad == 4);

    p.onPad(6, 90, false, true, 30.0, false); // a source that did not push: posted after the curve
    REQUIRE(w.triggers().size() == 2);
    CHECK(w.triggers()[1].track == 6);
    CHECK(w.triggers()[1].velocity == 90);
    CHECK(w.triggers()[1].pad == 6);

    p.onModeKey(PadMode::Track);
    p.onPad(9, 90, false, true, 40.0, false);
    CHECK(w.selection.selected() == 9);
    CHECK(w.triggers().size() == 2); // no sound
    p.onModeKey(PadMode::Scale);
    p.onPad(2, 90, true, true, 50.0, false); // SHIFT + pad selects in SCALE mode, no note
    CHECK(w.selection.selected() == 2);
    CHECK(w.triggers().size() == 2);
    CHECK(w.lastParam("pads.shift") == 1.0f);
    p.onShift(false);
    p.onPad(5, 90, false, true, 60.0, false); // SCALE: the pad plays the selected track (the engine maps the degree)
    CHECK(w.selection.selected() == 2);
    CHECK(p.lastHitMs(2) == 60.0);
    CHECK(w.triggers().size() == 3);
    // TRK-041: the rotary wraps 16 → 1 and 1 → 16; a selection made elsewhere reaches the engine on sync().
    CHECK(ui::input::pads::wrapTrack(15, 1) == 0);
    CHECK(ui::input::pads::wrapTrack(0, -1) == 15);
    w.selection.select(11);
    p.sync();
    CHECK(w.lastParam("pads.selected") == 11.0f);
}

TEST_CASE("trigger.pads: MUTE toggles the pattern's mute; SHIFT + pad mutes the other 15 in one undo step (PAD-006)",
          "[trigger]")
{
    World w;
    auto& p = *w.pads;
    const auto muted = [&](int t) { return w.project().scenes[0].patterns[0].trackMutes[static_cast<std::size_t>(t)]; };
    p.onModeKey(PadMode::Mute);
    p.onPad(3, 100, false, true, 0.0);
    CHECK(muted(3));
    CHECK_FALSE(w.project().scenes[0].tracks[3].mute); // the pattern's mute, not the track-level one
    CHECK(cmd::track::isMuted(w.project(), 0, 3));
    p.onPad(3, 100, false, true, 0.0);
    CHECK_FALSE(muted(3));
    CHECK(w.triggers().empty()); // MUTE mode never sounds
    const auto depth = cmd::history(w.bus).undoDepth();

    p.onPad(7, 100, true, true, 0.0); // SHIFT + pad 8
    for (int t = 0; t < 16; ++t)
        CHECK(muted(t) == (t != 7));
    CHECK(cmd::history(w.bus).undoDepth() == depth + 1);
    CHECK(cmd::history(w.bus).undoLabel() == "MUTE");
    CHECK(w.selection.selected() == 0); // SHIFT + pad does not select in MUTE mode

    p.onPad(7, 100, true, true, 0.0); // the same again unmutes all
    for (int t = 0; t < 16; ++t)
        CHECK_FALSE(muted(t));
    CHECK(cmd::history(w.bus).undoDepth() == depth + 2);

    REQUIRE(w.bus.undo()); // back to "all but 8 muted" in one step …
    for (int t = 0; t < 16; ++t)
        CHECK(muted(t) == (t != 7));
    REQUIRE(w.bus.undo()); // … and to nothing muted in one more
    for (int t = 0; t < 16; ++t)
        CHECK_FALSE(muted(t));
}

TEST_CASE("trigger.pads: each pattern keeps its own mutes; switching patterns restores them", "[trigger]")
{
    World w;
    auto& p = *w.pads;
    const auto isMuted = [&](int t) { return cmd::track::isMuted(w.project(), 0, t); };
    p.onModeKey(PadMode::Mute);
    p.onPad(2, 100, false, true, 0.0); // pattern 1: track 3 muted
    w.bus.execute(cmd::scene::setCurrentScenePattern(0, 1));
    CHECK_FALSE(isMuted(2)); // pattern 2 does not inherit pattern 1's mutes
    p.onPad(5, 100, false, true, 0.0); // pattern 2: track 6 muted
    CHECK(isMuted(5));
    w.bus.execute(cmd::scene::setCurrentScenePattern(0, 0));
    CHECK(isMuted(2));
    CHECK_FALSE(isMuted(5));
    w.bus.execute(cmd::scene::setCurrentScenePattern(0, 1));
    CHECK_FALSE(isMuted(2));
    CHECK(isMuted(5));
    // SHIFT + pad looks at the current pattern only: pattern 1's single mute does not count.
    p.onPad(5, 100, true, true, 0.0);
    for (int t = 0; t < 16; ++t)
        CHECK(isMuted(t) == (t != 5));
    w.bus.execute(cmd::scene::setCurrentScenePattern(0, 0));
    for (int t = 0; t < 16; ++t)
        CHECK(isMuted(t) == (t == 2));
}

TEST_CASE("trigger.pads: unmuting a track-level mute unmutes only the current pattern", "[trigger]")
{
    World w;
    auto& p = *w.pads;
    auto& s = cmd::project(w.bus).scenes[0];
    s.tracks[4].mute = true; // an imported muteSetting: muted in every pattern
    cmd::project(w.bus).settings.currentPattern = 3;
    REQUIRE(cmd::track::isMuted(w.project(), 0, 4));
    p.onModeKey(PadMode::Mute);
    p.onPad(4, 100, false, true, 0.0);
    CHECK_FALSE(cmd::track::isMuted(w.project(), 0, 4));
    CHECK_FALSE(s.tracks[4].mute);
    CHECK(w.lastParam("track.mute", 4) == 0.0f); // the engine drops the track-level mute …
    for (int q = 0; q < 16; ++q)
        CHECK(s.patterns[static_cast<std::size_t>(q)].trackMutes[4] == (q != 3)); // … the other patterns keep it
    REQUIRE(w.bus.undo()); // one undo step
    CHECK(s.tracks[4].mute);
    for (const auto& pat : s.patterns)
        CHECK_FALSE(pat.trackMutes[4]);
}

TEST_CASE("trigger.commands: track commands are undoable SetParams that reach the engine", "[trigger]")
{
    World w;
    w.bus.execute(cmd::track::setMode(2, 5, core::model::TrackMode::Thru));
    CHECK(w.project().scenes[2].tracks[5].mode == core::model::TrackMode::Thru);
    CHECK(w.lastParam("track.mode", (2 << 4) | 5) == 2.0f);
    CHECK(cmd::history(w.bus).undoLabel() == "TRACK TYPE");
    w.bus.execute(cmd::track::setTriggerMode(0, 1, core::model::TriggerMode::Gate));
    CHECK(w.lastParam("track.triggerMode", 1) == 1.0f);
    w.bus.execute(cmd::track::setChokeGroup(0, 1, 12)); // clamped to 8
    CHECK(w.project().scenes[0].tracks[1].chokeGroup == 8);
    CHECK(w.lastParam("track.chokeGroup", 1) == 8.0f);
    w.bus.execute(cmd::track::setPadColour(0, 1, 9));
    CHECK(w.project().scenes[0].tracks[1].colour == 9);
    w.bus.execute(cmd::track::setPadVelocity(0, 1, false));
    CHECK_FALSE(w.project().scenes[0].tracks[1].ampEnv.velocitySensitive);
    CHECK(w.lastParam("track.padVelocity", 1) == 0.0f);
    REQUIRE(w.bus.undo());
    CHECK(w.project().scenes[0].tracks[1].ampEnv.velocitySensitive);
    REQUIRE(w.bus.undo());
    REQUIRE(w.bus.undo());
    REQUIRE(w.bus.undo());
    REQUIRE(w.bus.undo()); // TRK-002: the mode change is undoable, the other mode's settings were never touched
    CHECK(w.project().scenes[2].tracks[5].mode == core::model::TrackMode::Sample);
    CHECK(w.lastParam("track.mode", (2 << 4) | 5) == 0.0f);
}

TEST_CASE("trigger.pads: lamps show colour, flash, muted pads off and mode keys at most 60 times a second (PAD-003)",
          "[trigger]")
{
    World w;
    auto& p = *w.pads;
    Lamps lamps;
    const engine::MeterFeed meters {};
    p.paintLamps(lamps, 0.0, meters);
    REQUIRE(lamps.flushes == 1);
    CHECK(lamps.lamps[io::pad(1)].second == 255); // empty tracks light too: track 1 is selected, full (UI-151)
    CHECK(lamps.lamps[io::pad(2)].second == io::kDimLevel); // an empty, unselected track dims
    CHECK(lamps.lamps[io::Control::PadModeTrack].second == 0);

    cmd::project(w.bus).scenes[0].tracks[0].playback.sample.id = "abc"; // track 1 has a sample
    cmd::project(w.bus).scenes[0].tracks[1].playback.sample.id = "def";
    p.onPad(0, 100, false, true, 100.0);
    p.paintLamps(lamps, 100.0, meters);
    CHECK(lamps.lamps[io::pad(1)].first == 0xFFFFFF); // the hit flashes white …
    p.paintLamps(lamps, 1200.0, meters);
    CHECK(lamps.lamps[io::pad(1)].first != 0xFFFFFF); // … and fades back to the track colour within ~1 s
    CHECK(lamps.lamps[io::pad(1)].second > 0);
    const int before = lamps.flushes;
    p.paintLamps(lamps, 1205.0, meters); // < 1/60 s later: no new lamp frame
    CHECK(lamps.flushes == before);

    p.onModeKey(PadMode::Mute);
    p.onPad(1, 100, false, true, 2000.0); // mute track 2
    p.paintLamps(lamps, 3500.0, meters);
    CHECK(lamps.lamps[io::Control::PadModeMute].second == 255);
    CHECK(lamps.lamps[io::pad(2)].second == 0); // muted = switched off (track 2's pad dark)
    CHECK(lamps.lamps[io::pad(1)].second > 0);
}

TEST_CASE("trigger.pads: MUTE held + pad toggles SOLO, SHIFT mutes the others, the release restores the mode",
          "[trigger]")
{
    World w;
    auto& p = *w.pads;
    using Kind = io::ControlEvent::Kind;
    const auto key = [&](bool press) {
        p.handle({press ? Kind::Press : Kind::Release, io::Control::PadModeMute, 0, io::kSourcePanel, 0.0});
    };
    const auto& tracks = [&]() -> const auto& { return w.project().scenes[0].tracks; };
    for (auto& t : cmd::project(w.bus).scenes[0].tracks)
        t.playback.sample.id = "abc"; // pads with content light
    // From PLAY: hold MUTE, solo tracks 3 and 5, no sound; the release goes back to PLAY.
    key(true);
    CHECK(p.soloHeld());
    CHECK(w.lastParam("pads.mode") == static_cast<float>(PadMode::Mute)); // the engine keeps the pads silent
    w.bridge.posted.clear();
    p.onPad(2, 100, false, true, 0.0, false);
    p.onPad(4, 100, false, true, 0.0, false);
    CHECK(tracks()[2].solo);
    CHECK(tracks()[4].solo);
    CHECK_FALSE(tracks()[2].mute);
    for (const auto& t : w.triggers())
        CHECK(t.velocity == 0); // releases only, never a hit
    // Lamps: the MUTE key blinks fast; soloed pads breathe up to full, the others (silent) faint.
    Lamps on, off, peak;
    p.paintLamps(on, 0.0, {});
    p.paintLamps(off, 1000.0 / 12.0 + 1.0, {});
    p.paintLamps(peak, io::kPulseMs / 2, {});
    CHECK(on.lamps[io::Control::PadModeMute].second != off.lamps[io::Control::PadModeMute].second);
    CHECK(on.lamps[io::pad(3)].second == io::kDimLevel); // the breath's low point (track 3 is not selected) …
    CHECK(peak.lamps[io::pad(3)].second == 255);         // … and its peak
    CHECK(on.lamps[io::pad(1)].second > 0);
    CHECK(on.lamps[io::pad(1)].second <= io::kFaintLevel);
    // SHIFT + pad while held: mutes the other tracks (one undo step), as in MUTE mode.
    p.onPad(0, 100, true, true, 0.0, false);
    CHECK_FALSE(cmd::track::isMuted(w.project(), 0, 0));
    CHECK(cmd::track::isMuted(w.project(), 0, 1));
    key(false);
    CHECK_FALSE(p.soloHeld());
    CHECK(p.mode() == PadMode::Play);
    CHECK(w.lastParam("pads.mode") == static_cast<float>(PadMode::Play));
    p.onPad(2, 100, false, true, 0.0, false); // MUTE released: PLAY again
    CHECK(tracks()[2].solo);
    // A plain tap still switches MUTE mode on; held from MUTE mode + pad, the release keeps MUTE mode.
    key(true);
    key(false);
    CHECK(p.mode() == PadMode::Mute);
    key(true);
    p.onPad(2, 100, false, true, 0.0, false);
    CHECK_FALSE(tracks()[2].solo);
    key(false);
    CHECK(p.mode() == PadMode::Mute);
    // OVER 1sec: a solo within the hold time does not switch the mode.
    p.onModeKey(PadMode::Play);
    p.setModeSelectHoldMs(1000.0);
    key(true);
    p.tick(100.0);
    p.onPad(6, 100, false, true, 0.0, false);
    p.tick(2000.0);
    CHECK(p.mode() == PadMode::Play);
    key(false);
    CHECK(p.mode() == PadMode::Play);
    CHECK(tracks()[6].solo);
}

// The engine's hits flash the pads (observeHits): a sequencer step on track n flashes pad n in PLAY / TRACK / MUTE,
// the first observation only takes the counts, a muted track's pad stays off, and an empty track's pad stays lit
// without flashing.
TEST_CASE("trigger.pads: the engine's hits (sequencer steps) flash the pads like a press (UI-151)", "[trigger]")
{
    World w;
    auto& p = *w.pads;
    for (auto& t : cmd::project(w.bus).scenes[0].tracks)
        t.playback.sample.id = "abc";
    engine::PlayheadFeed ph {};
    ph.trackHits[3] = 41; // counts from before: no flash
    p.observeHits(ph, 0.0);
    Lamps lamps;
    p.paintLamps(lamps, 10.0, {});
    CHECK(lamps.lamps[io::pad(4)].first != 0xFFFFFF);
    CHECK(lamps.lamps[io::pad(4)].second == io::kDimLevel);
    ph.trackHits[3] = 42; // a sequencer step on track 4
    p.observeHits(ph, 100.0);
    p.paintLamps(lamps, 100.0, {});
    CHECK((lamps.lamps[io::pad(4)].first == 0xFFFFFF && lamps.lamps[io::pad(4)].second == 255));
    CHECK(lamps.lamps[io::pad(5)].second == io::kDimLevel);
    p.paintLamps(lamps, 1200.0, {});
    CHECK((lamps.lamps[io::pad(4)].first != 0xFFFFFF && lamps.lamps[io::pad(4)].second == io::kDimLevel));
    p.onModeKey(PadMode::Track); // the same in TRACK mode …
    ph.trackHits[3] = 43;
    p.observeHits(ph, 1300.0);
    p.paintLamps(lamps, 1300.0, {});
    CHECK(lamps.lamps[io::pad(4)].first == 0xFFFFFF);
    cmd::project(w.bus).scenes[0].tracks[3].mute = true; // … but a muted track does not flash
    ph.trackHits[3] = 44;
    p.observeHits(ph, 2000.0);
    p.paintLamps(lamps, 2000.0, {});
    CHECK(lamps.lamps[io::pad(4)].second == 0);          // the muted track's pad: off, no flash
    p.hasContent = [](int track) { return track != 4; }; // the app's content answer (the shell's track summary)
    ph.trackHits[4] = 1;                                 // a hit reported on the empty track 5 …
    ph.trackHits[5] = 1;                                 // … and on track 6, which has content
    p.observeHits(ph, 2100.0);
    p.paintLamps(lamps, 2100.0, {});
    CHECK((lamps.lamps[io::pad(5)].first != 0xFFFFFF && lamps.lamps[io::pad(5)].second == io::kDimLevel));
    CHECK((lamps.lamps[io::pad(6)].first == 0xFFFFFF && lamps.lamps[io::pad(6)].second == 255));
}
