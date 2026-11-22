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

TEST_CASE("midi.model: the project's MIDI values reach the engine (non-default values only)", "[midi]")
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
    CHECK_FALSE(value("midi.in.channel", 3, v)); // default (track 4 → channel 4): not posted
    CHECK(bridge.posted.size() == 4);

    // And the engine takes them (a MIDI-mode track 8 of scene 3 then has the chord and program).
    auto e = miditest::make();
    REQUIRE(io::midi::postMidiSettings(p, e->bridge(), core::builtinParams()));
    miditest::Driver d {*e, 64};
    d.blocks(2);
    CHECK(e->midi().track(2, 7).notes[1] == 64);
    CHECK(e->midi().track(2, 7).program == 12);
    CHECK(e->midi().receiveChannel(4) == 0);
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
