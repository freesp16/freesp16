// Library commands and load-to-track (LIB-012, LIB-013, EDT-U5): loadSample is one undo step that restores the previous
// sample reference and the values LIB-012 resets; the engine follows through the bridge hook (TrackSampleSync); the
// browse.* ParamSpecs are registered.
#include "LibTestSupport.h"

#include "core/audio/SampleBufferStorage.h"
#include "core/commands/CommandBusAccess.h"
#include "core/commands/UndoStack.h"
#include "core/commands/library/LibraryCommands.h"
#include "core/model/ModelOps.h"
#include "core/params/ParamRegistry.h"
#include "services/library/SidecarStore.h"
#include "services/library/TrackLoader.h"
#include "services/library/TrackSampleSync.h"

#include <catch2/catch_test_macros.hpp>

using namespace services::library;
namespace cmd = core::commands::library;
using core::model::SampleRef;

namespace
{
core::model::Track& track(core::CommandBus& bus, int s, int t)
{
    return core::commands::project(bus).scenes[static_cast<std::size_t>(s)].tracks[static_cast<std::size_t>(t)];
}
} // namespace

TEST_CASE("lib.commands: loadSample sets the LIB-012 values and undo restores the previous sample",
          "[library][commands]")
{
    core::CommandBus bus;
    core::commands::replaceProject(bus, core::model::newDefaultProject());
    auto& t = track(bus, 1, 4);
    t.playback.start = 100, t.playback.loopStart = 200, t.playback.length = 300;
    t.playback.pitchCents = 700; // kept (LIB-012: only the sample and the listed values change)
    t.volume = 90;

    const SampleRef kick{"sha256:k", "$Lib/Samples/kick.wav", "kick", false, {}};
    bus.execute(cmd::loadSample(1, 4, kick, {44100, false}));
    CHECK(t.playback.sample.id == "sha256:k");
    CHECK(t.playback.start == 0);
    CHECK(t.playback.loopStart == 0);
    CHECK(t.playback.length == 44100);
    CHECK(t.playback.stretchMode == core::model::StretchMode::Off);
    CHECK(t.ampEnv.velocitySensitive);
    CHECK(t.playback.pitchCents == 700);
    CHECK(t.volume == 90);
    CHECK(core::commands::project(bus).sampleRefs.find({"sha256:k"}) != nullptr);
    CHECK(core::commands::history(bus).undoLabel() == "SAMPLE");

    const SampleRef loop{"sha256:l", "$Lib/Samples/loop.wav", "loop", false, {}};
    bus.execute(cmd::loadSample(1, 4, loop, {88200, true}));
    CHECK(t.playback.sample.id == "sha256:l");
    CHECK(t.playback.stretchMode == core::model::StretchMode::MasterTempo);
    CHECK_FALSE(t.ampEnv.velocitySensitive);
    CHECK(core::commands::history(bus).undoDepth() == 2); // two loads, two steps

    REQUIRE(bus.undo());
    CHECK(t.playback.sample.id == "sha256:k");
    CHECK(t.playback.length == 44100);
    CHECK(t.playback.stretchMode == core::model::StretchMode::Off);
    CHECK(t.ampEnv.velocitySensitive);
    CHECK(core::commands::project(bus).sampleRefs.find({"sha256:l"}) == nullptr); // the record, never the file
    REQUIRE(bus.undo());
    CHECK(t.playback.sample.empty());
    CHECK(t.playback.start == 100);
    CHECK(t.playback.loopStart == 200);
    CHECK(t.playback.length == 300);
    CHECK(core::commands::project(bus).sampleRefs.entries.empty());
    REQUIRE(bus.redo());
    CHECK(t.playback.sample.id == "sha256:k");

    bus.execute(cmd::unloadSample(1, 4));
    CHECK(t.playback.sample.empty());
    CHECK(t.playback.length == 44100); // unload keeps the settings
    REQUIRE(bus.undo());
    CHECK(t.playback.sample.id == "sha256:k");
}

TEST_CASE("lib.commands: loadFolder loads the first 16 onto tracks 1-16 as one step", "[library][commands]")
{
    core::CommandBus bus;
    core::commands::replaceProject(bus, core::model::newDefaultProject());
    std::vector<SampleRef> refs;
    for (int i = 0; i < 20; ++i)
        refs.push_back({"sha256:" + std::to_string(i), "$Lib/Samples/f/" + std::to_string(i) + ".wav", "x", false, {}});
    std::vector<cmd::LoadDefaults> d(20, cmd::LoadDefaults{1000, false});
    bus.execute(cmd::loadFolder(2, refs, d));
    for (int t = 0; t < 16; ++t)
    {
        CHECK(track(bus, 2, t).playback.sample.id == "sha256:" + std::to_string(t));
        CHECK(track(bus, 2, t).playback.length == 1000);
    }
    CHECK(core::commands::project(bus).sampleRefs.entries.size() == 16);
    CHECK(core::commands::history(bus).undoDepth() == 1);
    REQUIRE(bus.undo());
    for (int t = 0; t < 16; ++t)
        CHECK(track(bus, 2, t).playback.sample.empty());

    bus.execute(cmd::loadFolder(0, std::span<const SampleRef>(refs.data(), 3)));
    CHECK(track(bus, 0, 2).playback.sample.id == "sha256:2");
    CHECK(track(bus, 0, 3).playback.sample.empty());
}

TEST_CASE("lib.commands: the one-shot / loop classifier", "[library][commands]")
{
    CHECK_FALSE(cmd::classify(44100, 44100, {}, {}, false).loop);     // 1 s, no loop point
    CHECK_FALSE(cmd::classify(44100, 44100, {}, {}, true).loop);      // < 1 bar at 120 BPM
    CHECK(cmd::classify(88200, 44100, {}, {}, true).loop);            // 1 bar + loop point
    CHECK_FALSE(cmd::classify(88200 * 4, 44100, {}, {}, false).loop); // long but no loop point
    CHECK(cmd::classify(1000, 44100, 128.0, {}, false).loop);         // sidecar BPM
    CHECK(cmd::classify(1000, 44100, {}, 2.0, false).loop);           // sidecar bars
    CHECK(cmd::classify(-5, 44100, {}, {}, false).frames == 0);
}

TEST_CASE("lib.load: load to track decodes, executes one step, the engine follows undo and redo", "[library][commands]")
{
    libtest::Fixture f;
    const auto kick = libtest::writeSample(f.samples().getChildFile("Drums").getChildFile("kick.wav"), 4410, 100);
    const auto snare = libtest::writeSample(f.samples().getChildFile("Drums").getChildFile("snare.wav"), 2205, 300);
    REQUIRE(SidecarStore().write(snare, core::model::SampleMetadata{1.0, 120.0, 7, 0, 0, 0, {}}).ok());
    libtest::SwapEngine engine;
    core::CommandBus bus;
    core::commands::replaceProject(bus, core::model::newDefaultProject());
    TrackSampleSync sync(f.pool, engine);
    core::commands::setBridge(bus,
                              [&](const core::ChangeSet& c, const core::model::Project& p) { sync.onChange(c, p); });
    TrackLoader loader(f.library, f.pool, bus);

    bool loaded = false;
    loader.load(0, 5, kick, [&](core::Result<void> r, const LoadInfo&) { loaded = r.ok(); });
    f.workers.drain();
    REQUIRE(loaded);
    auto& t = track(bus, 0, 5);
    CHECK(t.playback.sample.id.rfind("sha256:", 0) == 0);
    const auto* ref = core::commands::project(bus).sampleRefs.find(t.playback.sample);
    REQUIRE(ref != nullptr);
    CHECK(ref->path == "$Lib/Samples/Drums/kick.wav");
    CHECK(ref->displayName == "kick");
    CHECK(t.playback.length == 4410);
    const auto* kickBuffer = sync.current(0, 5);
    REQUIRE(kickBuffer != nullptr);
    CHECK(engine.track(0, 5) == kickBuffer);

    loader.load(0, 5, snare);
    f.workers.drain();
    const auto* snareBuffer = sync.current(0, 5);
    REQUIRE(snareBuffer != nullptr);
    CHECK(snareBuffer != kickBuffer);
    CHECK(t.playback.length == 2205);
    CHECK(t.playback.stretchMode == core::model::StretchMode::MasterTempo); // sidecar BPM → loop defaults
    CHECK(engine.track(0, 5) == snareBuffer);

    // Undo: the previous sample comes back on the track and in the engine (from the pool's cache: no decode).
    REQUIRE(bus.undo());
    f.workers.drain();
    CHECK(sync.current(0, 5) == kickBuffer);
    CHECK(engine.track(0, 5) == kickBuffer);
    CHECK(t.playback.length == 4410);
    REQUIRE(bus.redo());
    f.workers.drain();
    CHECK(engine.track(0, 5) == snareBuffer);

    // Load to the pad pressed next.
    loader.arm(kick);
    CHECK(loader.onPadPressed(0, 9));
    CHECK_FALSE(loader.onPadPressed(0, 10));
    f.workers.drain();
    CHECK(engine.track(0, 9) == kickBuffer);

    // LIB-013 from a folder, A→Z.
    bool folderOk = false;
    loader.loadFolder(1, f.samples().getChildFile("Drums"), [&](core::Result<void> r) { folderOk = r.ok(); });
    f.workers.drain();
    REQUIRE(folderOk);
    CHECK(engine.track(1, 0) == kickBuffer);
    CHECK(engine.track(1, 1) == snareBuffer);
    CHECK(f.pool.bufferCount() == 2);

    // A missing file fails without touching the project.
    bool failed = false;
    loader.load(0, 6, f.samples().getChildFile("none.wav"),
                [&](core::Result<void> r, const LoadInfo&) { failed = ! r.ok(); });
    f.workers.drain();
    CHECK(failed);
    CHECK(track(bus, 0, 6).playback.sample.empty());
    core::commands::setBridge(bus, {});
}

TEST_CASE("lib.load: unloading a live take preserves the underlying and replacement samples", "[library]")
{
    libtest::Fixture f;
    const auto kick = libtest::writeSample(f.samples().getChildFile("kick.wav"), 4410, 100);
    const auto snare = libtest::writeSample(f.samples().getChildFile("snare.wav"), 2205, 300);
    std::unique_ptr<core::audio::SampleBuffer, core::audio::SampleBufferDeleter> live(
        core::audio::allocateSampleBuffer(128, 1, 44100.0));
    REQUIRE(live);
    libtest::SwapEngine engine;
    core::CommandBus bus;
    core::commands::replaceProject(bus, core::model::newDefaultProject());
    TrackSampleSync sync(f.pool, engine);
    core::commands::setBridge(bus, [&](const auto& c, const auto& p) { sync.onChange(c, p); });
    TrackLoader loader(f.library, f.pool, bus);
    loader.load(0, 5, kick);
    f.workers.drain();
    REQUIRE(engine.track(0, 5));
    REQUIRE(sync.installLive(0, 5, live.get()));
    REQUIRE(engine.track(0, 5) == live.get());
    SECTION("unload restores the underlying file")
    {
        REQUIRE(sync.removeLive(0, 5, live.get(), core::commands::project(bus)));
        f.workers.drain();
        REQUIRE(engine.track(0, 5));
        CHECK(engine.track(0, 5)->frames == 4410);
    }
    SECTION("unload leaves a replacement file in place")
    {
        loader.load(0, 5, snare);
        f.workers.drain();
        const auto* replacement = engine.track(0, 5);
        REQUIRE(replacement);
        REQUIRE(replacement->frames == 2205);
        REQUIRE(sync.removeLive(0, 5, live.get(), core::commands::project(bus)));
        CHECK(engine.track(0, 5) == replacement);
    }
    CHECK_FALSE(track(bus, 0, 5).playback.sample.empty());
    core::commands::setBridge(bus, {});
}

TEST_CASE("lib.params: the browse catalogue is registered with its defaults", "[library][params]")
{
    const auto& reg = core::builtinParams();
    CHECK(reg.errors().empty());
    for (const char* id : {"browse.filterType", "browse.sort", "browse.preview", "browse.previewVolume",
                           "browse.previewMonitor", "browse.loadTo", "browse.info"})
    {
        INFO(id);
        const auto* spec = reg.find(id);
        REQUIRE(spec != nullptr);
        CHECK(spec->isPreference());
        CHECK_FALSE(spec->lockable);
        CHECK(spec->shortLabel.size() <= 8);
    }
    CHECK(reg.find("browse.previewVolume")->defaultValue == 50);
    CHECK(reg.find("browse.preview")->defaultValue == 1);
    char buf[32];
    reg.find("browse.info")->formatValue(75.5f, buf, sizeof buf);
    CHECK(std::string(buf) == "1:15");
}
