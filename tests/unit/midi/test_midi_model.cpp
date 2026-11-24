// The message-thread side of MIDI: incoming CCs write the model without an undo step
// (ui::ModelParamAccess::setRemote, the app's CC sink), the project's MIDI values reach the engine
// (io::midi::postMidiSettings, ProjectFeed's forward-all path) and the MIDI settings come from the app's settings.
#include "tests/unit/midi/MidiTestUtil.h"

#include "core/commands/CommandBusAccess.h"
#include "core/model/Project.h"
#include "io/midi/MidiSettings.h"
#include "tests/fakes/FakeEngineBridge.h"
#include "ui/shell/ModelParamAccess.h"

#include <catch2/catch_test_macros.hpp>

#include <variant>

TEST_CASE("midi.model: an incoming CC updates the model without an undo step", "[midi]")
{
    core::CommandBus bus;
    fakes::FakeEngineBridge bridge;
    core::commands::replaceProject(bus, std::make_unique<core::model::Project>());
    core::commands::setBridge(bus, [&](const core::ChangeSet& c, const core::model::Project& p) {
        ui::forwardModelChanges(c, p, bridge);
    });
    ui::ModelParamAccess values(bus, &bridge);
    const auto& vol = *core::builtinParams().find("track.volume");
    values.setRemote(vol, (1 << 4) | 2, 80.0f); // scene 2, track 3 (CC 7 on channel 3)
    CHECK(core::commands::project(bus).scenes[1].tracks[2].volume == 80);
    CHECK_FALSE(bus.undo()); // nothing to undo: the CC is not an edit of the user's
    CHECK(core::commands::project(bus).scenes[1].tracks[2].volume == 80);
    // An ordinary edit stays undoable, and undo does not bring back a pre-CC value.
    values.set(vol, (1 << 4) | 2, 90.0f);
    REQUIRE(bus.undo());
    CHECK(core::commands::project(bus).scenes[1].tracks[2].volume == 80);
}

TEST_CASE("midi.model: the project's MIDI values reach the engine (MIDI track values: non-default only)", "[midi]")
{
    core::model::Project p;
    p.midi.input.clock = false; // the default is ENABLE (utility.md; schema 2)
    p.midi.input.channels[4] = 0; // track 5 receive OFF
    p.scenes[2].tracks[7].midi.notes[1] = 64;
    p.scenes[2].tracks[7].midi.program = 12;
    fakes::FakeEngineBridge bridge;
    REQUIRE(io::midi::postMidiSettings(p, bridge, core::builtinParams()));
    auto value = [&](std::string_view id, std::uint16_t scope, float& v) {
        for (const auto& m : bridge.posted)
            if (const auto* c = std::get_if<engine::ParamChange>(&m); c && c->param == miditest::id(id) && c->scope == scope)
            {
                v = c->value;
                return true;
            }
        return false;
    };
    float v = 0;
    CHECK((value("midi.in.clock", 0, v) && v == 0.0f));
    CHECK((value("midi.in.channel", 4, v) && v == 0.0f));
    CHECK((value("midi.note2", (2 << 4) | 7, v) && v == 64.0f));
    CHECK((value("midi.program", (2 << 4) | 7, v) && v == 12.0f));
    CHECK((value("midi.in.channel", 3, v) && v == 4.0f)); // defaults too: every enable and channel (8 + 32)
    CHECK((value("midi.out.songPosition", 0, v) && v == 1.0f));
    CHECK(value("midi.mode", core::midi::kProjectOpenScope, v)); // the MIDI track values start from the defaults
    CHECK_FALSE(value("midi.note1", (2 << 4) | 7, v));           // default: not posted
    CHECK(bridge.posted.size() == 8 + 32 + 1 + 2);

    // And the engine takes them (a MIDI-mode track 8 of scene 3 then has the chord and program).
    auto e = miditest::make();
    REQUIRE(io::midi::postMidiSettings(p, e->bridge(), core::builtinParams()));
    miditest::Driver d {*e, 64};
    d.blocks(2);
    CHECK(e->midi().track(2, 7).notes[1] == 64);
    CHECK(e->midi().track(2, 7).program == 12);
    CHECK(e->midi().receiveChannel(4) == 0);
}

TEST_CASE("midi.model: opening another project replaces every MIDI value of the previous one", "[midi]")
{
    // Project A: track 3 receives CH.5, NOTE/CC DISABLE, track 1 sends on CH.9, a chord and CC on scene 3 track 8.
    core::model::Project a;
    a.midi.input.channels[2] = 5;
    a.midi.input.noteCc = false;
    a.midi.output.channels[0] = 9;
    a.scenes[2].tracks[7].midi.notes[1] = 64;
    a.scenes[2].tracks[7].midi.cc2[0].value = 99;
    a.scenes[2].tracks[7].midi.cc2[0].active = true;
    const core::model::Project b; // project B: all defaults (CH.n, ENABLE)
    auto e = miditest::make();
    miditest::Driver d{*e, 64};
    REQUIRE(io::midi::postMidiSettings(a, e->bridge(), core::builtinParams()));
    d.blocks(2);
    REQUIRE(e->midi().receiveChannel(2) == 5);
    REQUIRE(e->midi().track(2, 7).notes[1] == 64);
    auto noteHeard = [&]
    {
        d.hits.clear();
        REQUIRE(e->midi().hub().pushIn(
            0, miditest::msg(0x90, 0, 100, miditest::hostNs(static_cast<double>(d.frame()) - 1))));
        d.blocks(2);
        return ! d.hits.empty();
    };
    CHECK_FALSE(noteHeard()); // NOTE/CC DISABLE
    REQUIRE(io::midi::postMidiSettings(b, e->bridge(), core::builtinParams()));
    d.blocks(2);
    CHECK(e->midi().receiveChannel(2) == 3);
    CHECK(noteHeard()); // ENABLE again
    const auto& m = e->midi().track(2, 7);
    CHECK(m.notes[1] == 0); // NOTE2 off (a semitone offset, 0 = off)
    CHECK(m.ccValue[4] == 0);
    CHECK_FALSE(m.ccActive[4]);
    // The send channel too: a MIDI track 1 of B sends on CH.1, not A's CH.9.
    e->midi().hub().setOutputConnected(0, true);
    d.param("track.mode", 0, 1.0f);
    miditest::drainOut(*e);
    e->triggers().push(engine::TriggerNow{0, 100, 60, 0});
    d.blocks(2);
    const auto out = miditest::drainOut(*e);
    REQUIRE(! out.empty());
    CHECK(out[0].bytes[0] == 0x90);
}

TEST_CASE("midi.settings: read from the app's settings, the environment wins", "[midi]")
{
    io::midi::configure({{"midi_spin_us", "250"}, {"midi_providers", "virtual"}});
    CHECK(io::midi::setting("midi_spin_us", "100") == "250");
    CHECK(io::midi::setting("midi_providers") == "virtual");
    CHECK(io::midi::setting("midi_virtual", "none") == "none");
    io::midi::configure({});
    CHECK(io::midi::setting("midi_spin_us", "100") == "100");
}
