// ui::ModelParamAccess, forwardModelChanges / forwardAllModel and fillTrackSummaries (the composition root's model
// side): knob edits become undoable, coalesced commands whose bridge hook reaches the engine; values without
// a model field stay local; model conversions (pitch cents, envelope storage); the HOME summaries come from the model.
#include "core/commands/CommandBusAccess.h"
#include "core/commands/UndoStack.h"
#include "core/commands/library/LibraryCommands.h"
#include "core/model/Project.h"
#include "core/params/ParamRegistry.h"
#include "services/store/NativeCodec.h"
#include "tests/fakes/FakeEngineBridge.h"
#include "ui/input/pads/PadController.h"
#include "ui/shell/ModelParamAccess.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <variant>

namespace
{
namespace cmd = core::commands;

struct World
{
    core::CommandBus bus;
    fakes::FakeEngineBridge bridge;
    std::unique_ptr<ui::ModelParamAccess> values;
    World()
    {
        cmd::replaceProject(bus, std::make_unique<core::model::Project>());
        cmd::setBridge(bus, [this](const core::ChangeSet& c, const core::model::Project& p) {
            ui::input::pads::forwardTrackChanges(c, p, bridge);
            ui::forwardModelChanges(c, p, bridge);
        });
        values = std::make_unique<ui::ModelParamAccess>(bus, &bridge);
    }
    const core::ParamSpec& spec(const char* id) { return *core::builtinParams().find(id); }
    core::model::Project& project() { return cmd::project(bus); }
    int posts(const char* id, std::uint16_t scope, float* last = nullptr)
    {
        const auto pid = core::builtinParams().idOf(id).value_or(core::ParamId{});
        int n = 0;
        for (const auto& m : bridge.posted)
            if (const auto* c = std::get_if<engine::ParamChange>(&m); c != nullptr && c->param == pid && c->scope == scope)
            {
                ++n;
                if (last != nullptr)
                    *last = c->value;
            }
        return n;
    }
};
} // namespace

TEST_CASE("ui.model: a knob edit is an undoable command that reaches the engine (EDT-U1/U4)", "[ui][model]")
{
    World w;
    const std::uint16_t scope = (0 << 4) | 4; // scene 1, track 5
    REQUIRE(w.values->modelBacked(w.spec("track.volume")));
    w.values->set(w.spec("track.volume"), scope, 90);
    CHECK(w.project().scenes[0].tracks[4].volume == 90);
    CHECK(w.values->get(w.spec("track.volume"), scope) == 90);
    float v = 0;
    CHECK(w.posts("track.volume", scope, &v) == 1);
    CHECK(v == 90);
    w.values->set(w.spec("track.volume"), scope, 80); // coalesces with the first edit (EDT-U2)
    CHECK(cmd::history(w.bus).undoDepth() == 1);
    REQUIRE(w.bus.undo());
    CHECK(w.project().scenes[0].tracks[4].volume == 100);
    CHECK(w.posts("track.volume", scope, &v) == 3); // undo is audible too
    CHECK(v == 100);
}

TEST_CASE("ui.model: model conversions and the track fields forwardTrackChanges owns", "[ui][model]")
{
    World w;
    const std::uint16_t scope = 2; // track 3
    w.values->set(w.spec("playback.pitch"), scope, -7.5f);
    CHECK(w.project().scenes[0].tracks[2].playback.pitchCents == -750);
    CHECK(w.values->get(w.spec("playback.pitch"), scope) == Catch::Approx(-7.5f));
    w.values->set(w.spec("env.release"), scope, 128); // INFINITY
    CHECK(w.project().scenes[0].tracks[2].ampEnv.release == 1.0);
    CHECK(w.values->get(w.spec("env.release"), scope) == 128);
    w.values->set(w.spec("env.attack"), scope, 64);
    CHECK(w.values->get(w.spec("env.attack"), scope) == 64);
    // track.mute: the MIXER toggle is the current pattern's mute (TrackCommands.h); the track-level field and the
    // engine's track.mute stay as they were (SceneSync posts pattern.mutes).
    w.values->set(w.spec("track.mute"), scope, 1);
    CHECK(w.project().scenes[0].patterns[0].trackMutes[2]);
    CHECK_FALSE(w.project().scenes[0].tracks[2].mute);
    CHECK(w.values->get(w.spec("track.mute"), scope) == 1);
    CHECK(w.posts("track.mute", scope) == 0);
    w.project().settings.currentPattern = 1;
    CHECK(w.values->get(w.spec("track.mute"), scope) == 0); // another pattern, its own mutes
    w.project().settings.currentPattern = 0;
    // A project parameter.
    w.values->set(w.spec("mixer.masterVolume"), 0, 64);
    CHECK(w.project().settings.masterVolume == 64);
    CHECK(w.posts("mixer.masterVolume", 0) == 1);
}

TEST_CASE("ui.model: parameters without a model field stay local and go straight to the engine", "[ui][model]")
{
    World w;
    const auto& grid = w.spec("playback.gridSnap");
    CHECK_FALSE(w.values->modelBacked(grid));
    w.values->set(grid, 1, 1);
    CHECK(w.values->get(grid, 1) == 1);
    CHECK(cmd::history(w.bus).undoDepth() == 0);
    CHECK(w.posts("playback.gridSnap", 1) == 1);
}

TEST_CASE("ui.model: a sample load forwards pad VELOCITY and the region; forwardAllModel syncs a project",
          "[ui][model]")
{
    World w;
    core::model::SampleRef ref{.id = "sha256:1", .path = "$Lib/Samples/kick.wav", .displayName = "kick.wav"};
    w.bus.execute(cmd::library::loadSample(0, 0, ref, {.frames = 8820, .loop = false}));
    float v = -1;
    CHECK(w.posts("playback.length", 0, &v) == 1);
    CHECK(v == 8820);
    CHECK(w.posts("track.padVelocity", 0, &v) == 1); // LIB-012 / TRK-032: a one-shot gets VELOCITY ON
    CHECK(v == 1);
    w.bridge.posted.clear();
    ui::forwardAllModel(w.project(), w.bridge);
    CHECK(w.posts("track.volume", 15) == 1);
    CHECK(w.posts("track.padColor", 15) == 1);
    CHECK(w.posts("mixer.masterVolume", 0) == 1);
    CHECK(w.bridge.posted.size() < engine::kMessageQueueSize);

    ui::UiState st;
    ui::fillTrackSummaries(st, w.project());
    CHECK(st.tracks[0].hasContent);
    CHECK(st.tracks[0].name == "kick.wav");
    CHECK(st.tracks[0].typeBadge == "SMPL");
    CHECK_FALSE(st.tracks[1].hasContent);
}

TEST_CASE("ui.model: transport.tempo is the project tempo: saved, undoable, posted, synced on open (SYN-002)",
          "[ui][model]")
{
    World w;
    const auto& tempo = w.spec("transport.tempo");
    REQUIRE(w.values->modelBacked(tempo));
    CHECK(w.values->get(tempo, 0) == Catch::Approx(120.0));
    w.values->set(tempo, 0, 123.4f);
    CHECK(w.project().settings.tempo == 123.4); // on the 0.1 grid, not the float's 123.40000152…
    float v = 0;
    CHECK(w.posts("transport.tempo", 0, &v) == 1);
    CHECK(v == Catch::Approx(123.4));
    REQUIRE(w.bus.undo());
    CHECK(w.project().settings.tempo == 120.0);
    w.project().settings.tempo = 98.5; // a project that opens with its own tempo
    w.bridge.posted.clear();
    ui::forwardAllModel(w.project(), w.bridge);
    CHECK(w.posts("transport.tempo", 0, &v) == 1);
    CHECK(v == Catch::Approx(98.5));
}

TEST_CASE("ui.model: THRU BYPASS is a saved, undoable model field (MIX-051)", "[ui][model]")
{
    World w;
    const auto& spec = w.spec("routing.thruBypass");
    const std::uint16_t scope = (2 << 4) | 9; // scene 3, track 10
    REQUIRE(w.values->modelBacked(spec));
    CHECK(w.values->get(spec, scope) == 0);
    w.values->set(spec, scope, 1);
    CHECK(w.values->get(spec, scope) == 1);
    float v = 0;
    CHECK(w.posts("routing.thruBypass", scope, &v) == 1);
    CHECK(v == 1);
    CHECK(w.values->get(spec, (2 << 4) | 8) == 0); // per track

    // Saved with the project (the native codec writes routing.thruBypass) and read back.
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("thru-bypass-" + juce::String(juce::Time::currentTimeMillis()));
    const auto bundle = dir.getChildFile("Thru.sixteen");
    REQUIRE(dir.createDirectory());
    const auto saved = services::store::NativeCodec::save(w.project(), bundle);
    INFO(saved.error().message << " " << saved.error().detail);
    REQUIRE(saved.ok());
    bool written = false;
    for (const auto& f : bundle.findChildFiles(juce::File::findFiles, true, "*.json"))
        written = written || f.loadFileAsString().contains("\"thruBypass\"");
    CHECK(written);
    auto back = std::make_unique<core::model::Project>();
    services::store::ImportReport report;
    REQUIRE(services::store::NativeCodec::readInto(bundle, *back, report).ok());
    CHECK(back->scenes[2].tracks[9].routing == w.project().scenes[2].tracks[9].routing);
    w.bridge.posted.clear();
    ui::forwardAllModel(*back, w.bridge, core::builtinParams(), 2); // scene 3 shown: posts it
    CHECK(w.posts("routing.thruBypass", scope, &v) == 1);
    CHECK(v == 1);
    dir.deleteRecursively();

    REQUIRE(w.bus.undo());
    CHECK(w.values->get(spec, scope) == 0);
    CHECK(w.project().scenes[2].tracks[9].routing.extras.empty()); // OFF leaves no trace in the file
}

TEST_CASE("ui.model: MUTE MODE is a saved, undoable, per-track model field posted to the mixer", "[ui][model]")
{
    World w;
    const auto& spec = w.spec("track.muteMode");
    const std::uint16_t scope = (0 << 4) | 4; // scene 1, track 5
    REQUIRE(w.values->modelBacked(spec));
    CHECK(w.values->get(spec, scope) == 0); // STOP SOUND by default
    w.values->set(spec, scope, 1);          // STOP STEPS
    CHECK(w.values->get(spec, scope) == 1);
    CHECK(w.values->get(spec, (0 << 4) | 5) == 0); // per track
    float v = 0;
    CHECK(w.posts("track.muteMode", scope, &v) == 1);
    CHECK(v == 1);

    // Saved with the project (the track's "muteMode") and read back, then posted on open.
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("mute-mode-" + juce::String(juce::Time::currentTimeMillis()));
    const auto bundle = dir.getChildFile("MuteMode.sixteen");
    REQUIRE(dir.createDirectory());
    const auto saved = services::store::NativeCodec::save(w.project(), bundle);
    INFO(saved.error().message << " " << saved.error().detail);
    REQUIRE(saved.ok());
    auto back = std::make_unique<core::model::Project>();
    services::store::ImportReport report;
    REQUIRE(services::store::NativeCodec::readInto(bundle, *back, report).ok());
    w.bridge.posted.clear();
    ui::forwardAllModel(*back, w.bridge, core::builtinParams(), 0);
    CHECK(w.posts("track.muteMode", scope, &v) == 1);
    CHECK(v == 1);
    CHECK(w.posts("track.muteMode", (0 << 4) | 5, &v) == 1);
    CHECK(v == 0);
    dir.deleteRecursively();

    REQUIRE(w.bus.undo());
    CHECK(w.values->get(spec, scope) == 0);
    CHECK(w.project().scenes[0].tracks[4].extras.empty()); // STOP SOUND leaves no trace in the file
}

TEST_CASE("ui.model: a MIDI track's NOTE, CC, BANK / PROG and SELECT CH edits are saved with the project",
          "[ui][model][midi]")
{
    World w;
    const std::uint16_t scope = (1 << 4) | 3; // scene 2, track 4
    for (const char* id : {"midi.note1", "midi.note2", "midi.velocity", "midi.length", "midi.cc1.value3",
                           "midi.cc2.active6", "midi.bankMsb", "midi.program"})
    {
        INFO(id);
        CHECK(w.values->modelBacked(w.spec(id)));
    }
    w.values->set(w.spec("midi.note1"), scope, 65.0f);
    w.values->set(w.spec("midi.note2"), scope, -5.0f); // NOTE2: 5 semitones under NOTE
    w.values->set(w.spec("midi.velocity"), scope, 90.0f);
    w.values->set(w.spec("midi.cc1.value3"), scope, 33.0f);
    w.values->set(w.spec("midi.program"), scope, 12.0f);
    const auto& m = w.project().scenes[1].tracks[3].midi;
    CHECK(m.notes[0] == 65);
    CHECK(m.notes[1] == -5);
    CHECK(m.velocity == 90);
    CHECK(m.cc1[0].value == 33);
    CHECK(m.program == 12);
    float v = 0;
    CHECK(w.posts("midi.note1", scope, &v) == 1); // and the engine hears it
    CHECK(v == 65.0f);

    // SELECT CH: the track's send channel is UTILITY's project value (CHANNEL MAPPING (MIDI OUT)).
    const auto& ch = w.spec("midi.out.channel");
    CHECK(w.values->get(ch, 3) == 4.0f); // default channel n = track n
    w.values->set(ch, 3, 9.0f);
    CHECK(w.project().midi.output.channels[3] == 9);
    CHECK(w.values->get(ch, 3) == 9.0f);

    // Saved and read back.
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("midi-track-" + juce::String(juce::Time::currentTimeMillis()));
    const auto bundle = dir.getChildFile("Midi.sixteen");
    REQUIRE(dir.createDirectory());
    REQUIRE(services::store::NativeCodec::save(w.project(), bundle).ok());
    auto back = std::make_unique<core::model::Project>();
    services::store::ImportReport report;
    REQUIRE(services::store::NativeCodec::readInto(bundle, *back, report).ok());
    CHECK(back->scenes[1].tracks[3].midi == m);
    CHECK(back->midi.output.channels[3] == 9);
    dir.deleteRecursively();

    REQUIRE(w.bus.undo()); // SELECT CH
    CHECK(w.project().midi.output.channels[3] == 4);
    REQUIRE(w.bus.undo()); // PROG CHANGE
    CHECK(w.project().scenes[1].tracks[3].midi.program == -1);
}
