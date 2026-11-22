// The project settings UTILITY edits (SET-001/002; services/settings/ProjectSettings.h) — each commit
// one undoable command on the bus that marks the project edited, saved with the project (the native codec round
// trip), CHANNEL MAPPING RESET, the value texts, and the engine forwarding under the registered ids.
#include "core/commands/CommandBusAccess.h"
#include "core/model/Project.h"
#include "core/params/ParamRegistry.h"
#include "services/settings/ProjectSettings.h"
#include "services/store/Migrations.h"
#include "services/store/NativeCodec.h"
#include "tests/fakes/FakeEngineBridge.h"

#include <catch2/catch_test_macros.hpp>

#include <variant>

namespace st = services::settings;
namespace m = core::model;

namespace
{
std::string text(const m::Project& p, std::string_view id, float v, int index = 0)
{
    char buf[64];
    const auto* f = st::findProjectField(id);
    REQUIRE(f != nullptr);
    const int n = st::formatProjectValue(p, *f, index, v, buf, sizeof buf);
    return std::string(buf, static_cast<std::size_t>(n));
}
} // namespace

TEST_CASE("settings: project fields read the model, defaults as the model's", "[settings]")
{
    m::Project p;
    const auto value = [&](std::string_view id, int i = 0) { return st::projectValue(p, *st::findProjectField(id), i); };
    CHECK(value("project.patternQuantize") == 2.0f); // PATTERN (M p.42)
    CHECK(text(p, "project.patternQuantize", 2) == "PATTERN");
    CHECK(value("seq.padStartsSequence") == 0.0f);
    CHECK(text(p, "seq.padStartsSequence", 0) == "OFF");
    CHECK(value("midi.in.noteCc") == 1.0f);
    CHECK(text(p, "midi.in.noteCc", 1) == "ENABLE");
    CHECK(value("midi.in.clock") == 1.0f); // utility.md: ENABLE (requested default, schema 2)
    CHECK(text(p, "midi.in.clock", 0) == "DISABLE");
    for (int t = 0; t < 16; ++t)
        CHECK(value("midi.in.channel", t) == static_cast<float>(t + 1));
    CHECK(text(p, "midi.out.channel", 0, 3) == "OFF");
    CHECK(text(p, "midi.out.channel", 12, 3) == "CH 12");
    // PITCH RANGE: N Bipolar Semitones; an imported asymmetric range shows the cents.
    p.ribbon.pitchBottomCents = -200;
    p.ribbon.pitchTopCents = 200;
    CHECK(value("settings.pitchRange") == 1.0f);
    CHECK(text(p, "settings.pitchRange", 1) == "2 Bipolar Semitones");
    CHECK(text(p, "settings.pitchRange", 12) == "24 Bipolar Semitones");
    p.ribbon.pitchTopCents = 700;
    CHECK(value("settings.pitchRange") == -1.0f);
    CHECK(text(p, "settings.pitchRange", -1) == "\xE2\x88\x92" "200 / +700 cents");
}

TEST_CASE("settings: a project setting commit is one undo step and marks the project edited", "[settings]")
{
    core::CommandBus bus;
    core::commands::replaceProject(bus, std::make_unique<m::Project>());
    const auto& p = core::commands::project(bus);
    const auto* clock = st::findProjectField("midi.in.clock");
    REQUIRE(clock != nullptr);
    CHECK(st::setProjectValue(p, *clock, 0, 1) == nullptr); // unchanged (ENABLE by default): no command
    bus.execute(st::setProjectValue(p, *clock, 0, 0));
    CHECK_FALSE(p.midi.input.clock);
    CHECK(bus.dirty());
    bus.execute(st::setProjectValue(p, *st::findProjectField("project.patternQuantize"), 0, 1));
    CHECK(p.settings.patternQuantize == m::QuantizeUnit::Bar);
    bus.execute(st::setProjectValue(p, *st::findProjectField("midi.out.channel"), 4, 0));
    CHECK(p.midi.output.channels[4] == 0);
    bus.execute(st::setProjectValue(p, *st::findProjectField("settings.pitchRange"), 0, 12));
    CHECK(p.ribbon.pitchTopCents == 2400);
    CHECK(p.ribbon.pitchBottomCents == -2400);
    bus.execute(st::setProjectValue(p, *st::findProjectField("seq.padStartsSequence"), 0, 1));
    CHECK(p.settings.padStartsSequence);
    // Each commit is its own undo step (no coalescing between two separate commits).
    CHECK(bus.undo());
    CHECK_FALSE(p.settings.padStartsSequence);
    CHECK(bus.undo());
    CHECK(p.ribbon.pitchTopCents == 1200);
    CHECK(bus.undo());
    CHECK(p.midi.output.channels[4] == 5);
    CHECK(bus.undo());
    CHECK(p.settings.patternQuantize == m::QuantizeUnit::Pattern);
    CHECK(bus.undo());
    CHECK(p.midi.input.clock);
}

TEST_CASE("settings: CHANNEL MAPPING RESET restores track n ↔ channel n in one step (SET-002)", "[settings]")
{
    core::CommandBus bus;
    core::commands::replaceProject(bus, std::make_unique<m::Project>());
    const auto& p = core::commands::project(bus);
    CHECK(st::resetChannelMapping(p, true) == nullptr); // already n ↔ n
    const auto* in = st::findProjectField("midi.in.channel");
    bus.execute(st::setProjectValue(p, *in, 0, 10));
    bus.execute(st::setProjectValue(p, *in, 7, 0));
    bus.execute(st::resetChannelMapping(p, true));
    for (int t = 0; t < 16; ++t)
        CHECK(p.midi.input.channels[static_cast<std::size_t>(t)] == t + 1);
    CHECK(bus.undo());
    CHECK(p.midi.input.channels[0] == 10);
    CHECK(p.midi.input.channels[7] == 0);
}

TEST_CASE("settings: project settings are saved with the project (native codec round trip, SET-001)", "[settings]")
{
    core::CommandBus bus;
    core::commands::replaceProject(bus, std::make_unique<m::Project>());
    const auto& p = core::commands::project(bus);
    bus.execute(st::setProjectValue(p, *st::findProjectField("midi.out.songPosition"), 0, 0));
    bus.execute(st::setProjectValue(p, *st::findProjectField("midi.in.channel"), 2, 9));
    bus.execute(st::setProjectValue(p, *st::findProjectField("project.patternQuantize"), 0, 0));
    bus.execute(st::setProjectValue(p, *st::findProjectField("settings.pitchRange"), 0, 1));
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("settings-codec-" + juce::String(juce::Random::getSystemRandom().nextInt64()));
    dir.createDirectory();
    const auto bundle = dir.getChildFile("Settings.sixteen");
    REQUIRE(services::store::NativeCodec::save(p, bundle));
    auto back = std::make_unique<m::Project>();
    services::store::ImportReport report;
    REQUIRE(services::store::NativeCodec::readInto(bundle, *back, report));
    dir.deleteRecursively();
    CHECK_FALSE(back->midi.output.songPosition);
    CHECK(back->midi.input.channels[2] == 9);
    CHECK(back->settings.patternQuantize == m::QuantizeUnit::Step);
    CHECK(back->ribbon.pitchTopCents == 200);
}

TEST_CASE("settings: changes reach the engine under the registered ids only", "[settings]")
{
    core::CommandBus bus;
    core::commands::replaceProject(bus, std::make_unique<m::Project>());
    const auto& p = core::commands::project(bus);
    fakes::FakeEngineBridge bridge;
    const auto& reg = core::builtinParams();
    core::ChangeSet changes;
    bus.execute(st::setProjectValue(p, *st::findProjectField("seq.padStartsSequence"), 0, 1));
    changes.add("settings.padStartsSequence");
    changes.add("midi.input.clock");          // a MIDI catalogue id: posted when that catalogue is built in
    changes.add("midi.output.channels[3]");
    st::forwardSettingsChanges(changes, p, bridge, reg);
    int pad = 0, midi = 0;
    for (const auto& msg : bridge.posted)
        if (const auto* c = std::get_if<engine::ParamChange>(&msg))
        {
            const auto& id = reg[c->param].id;
            pad += id == "seq.padStartsSequence" && c->value == 1.0f ? 1 : 0;
            midi += id.substr(0, 5) == "midi." ? 1 : 0;
        }
    CHECK(pad == 1);
    CHECK(midi == (reg.find("midi.in.clock") != nullptr ? 2 : 0));
}

TEST_CASE("settings: the spec's defaults (requested) and the v1 to v2 migration that keeps old projects",
          "[settings][migration]")
{
    // New projects: every MIDI enable is ENABLE (utility.md §Rows); SCENE BPM = ASK; MIDI OUT channel OFF allowed.
    m::Project p;
    for (const auto* port : {&p.midi.input, &p.midi.output})
        CHECK((port->clock && port->startStop && port->noteCc && port->songPosition));
    const auto& reg = core::builtinParams();
    for (const char* id : {"midi.in.clock", "midi.in.startStop", "midi.in.noteCc", "midi.in.songPosition",
                           "midi.out.clock", "midi.out.startStop", "midi.out.noteCc", "midi.out.songPosition"})
    {
        INFO(id);
        REQUIRE(reg.find(id) != nullptr);
        CHECK(reg.find(id)->defaultValue == 1.0f); // the engine starts where the model does
    }
    CHECK(reg.find("scene.bpmPolicy")->defaultValue == 1.0f); // ASK
    CHECK(reg.find("midi.out.channel")->minValue == 0.0f);    // OFF
    // A v1 file omitted the enables that were off (v1's default): the migration writes them out, so the project
    // opens as it was saved; values the file holds stay.
    juce::var root(new juce::DynamicObject());
    auto* project = new juce::DynamicObject();
    root.getDynamicObject()->setProperty("project", juce::var(project));
    auto* midi = new juce::DynamicObject();
    project->setProperty("midi", juce::var(midi));
    auto* output = new juce::DynamicObject();
    output->setProperty("clock", true);
    midi->setProperty("output", juce::var(output));
    services::store::setSchemaVersion(root, 1);
    const auto r = services::store::MigrationRunner::builtin().run(root, 1);
    REQUIRE(r.ok());
    CHECK(r.value() == services::store::kSchemaVersion);
    const auto in = root["project"]["midi"]["input"];
    const auto out = root["project"]["midi"]["output"];
    CHECK_FALSE(static_cast<bool>(in["clock"]));
    CHECK_FALSE(static_cast<bool>(in["startStop"]));
    CHECK_FALSE(static_cast<bool>(in["songPosition"]));
    CHECK_FALSE(in.getDynamicObject()->hasProperty("noteCc")); // unchanged default (true in both schemas)
    CHECK(static_cast<bool>(out["clock"]));                     // saved on: kept
    CHECK_FALSE(static_cast<bool>(out["startStop"]));
}
