// Autosave (PRJ-030/031, NFR-010) and the data-safety side of audio-device loss (NFR-012): intervals while
// stopped / playing, never the user's file, a copy that reads back, an immediate write when the device goes away,
// the transport state kept and resumed; with the real engine on the FakeAudioDevice, a drop-out stops the engine
// without a crash.
#include "ProjectTestSupport.h"

#include "engine/EngineApi.h"
#include "tests/fakes/FakeAudioDevice.h"
#include "tests/fakes/FakeEngineBridge.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace projtest;
using services::store::SaveKind;

TEST_CASE("project.autosave: every 60 s when stopped, every 5 min while playing, only when unsaved", "[project]")
{
    Env env("autosave-timing");
    REQUIRE(env.store().create("Timed").ok());
    REQUIRE(env.save(SaveKind::Save, {}).ok());
    auto& a = env.session->autosave();
    const auto file = a.file();
    CHECK(file == env.system().getChildFile("autosave/Timed.json"));

    a.tick(0, false);
    a.tick(120000, false);
    env.workers.drain();
    CHECK(a.writes() == 0); // clean: nothing to write

    setTempo(env.bus, 90.0); // first unsaved change at t = 200 s
    a.tick(200000, false);
    a.tick(259999, false);
    env.workers.drain();
    CHECK(a.writes() == 0);
    a.tick(260000, false);
    CHECK(a.busy());
    a.tick(260001, false); // no second write while one runs
    env.workers.drain();
    CHECK(a.writes() == 1);
    CHECK(file.existsAsFile());
    a.tick(400000, false); // nothing new since the write
    env.workers.drain();
    CHECK(a.writes() == 1);

    setTempo(env.bus, 91.0); // playing: 5 min
    a.tick(500000, true);
    a.tick(500000 + 299999, true);
    env.workers.drain();
    CHECK(a.writes() == 1);
    a.tick(800000, true);
    env.workers.drain();
    CHECK(a.writes() == 2);

    // Off (project.autosave) → never.
    a.setEnabled(false);
    setTempo(env.bus, 92.0);
    a.tick(900000, false);
    a.tick(2000000, false);
    env.workers.drain();
    CHECK(a.writes() == 2);
    a.setEnabled(true);

    // A user save makes the copy stale: removed.
    REQUIRE(env.save(SaveKind::Save, {}).ok());
    CHECK_FALSE(file.existsAsFile());
}

TEST_CASE("project.autosave: UTILITY AUTOSAVE 5 min waits 5 min while stopped too (settings.autosave)", "[project]")
{
    Env env("autosave-5min");
    REQUIRE(env.store().create("Five").ok());
    REQUIRE(env.save(SaveKind::Save, {}).ok());
    auto& a = env.session->autosave();
    env.session->setAutosaveIdleInterval(5 * 60000.0);
    CHECK(a.idleIntervalMs() == 300000.0);
    setTempo(env.bus, 95.0); // first unsaved change at t = 10 s
    a.tick(10000, false);
    a.tick(10000 + 60000, false); // the 1-min interval would write here
    a.tick(10000 + 299999, false);
    env.workers.drain();
    CHECK(a.writes() == 0);
    a.tick(10000 + 300000, false);
    env.workers.drain();
    CHECK(a.writes() == 1);
    // Playing: never sooner than the stopped interval, so a longer idle interval also holds while playing.
    env.session->setAutosaveIdleInterval(10 * 60000.0);
    setTempo(env.bus, 96.0);
    a.tick(400000, true);
    a.tick(400000 + 300000, true);
    env.workers.drain();
    CHECK(a.writes() == 1);
    a.tick(400000 + 600000, true);
    env.workers.drain();
    CHECK(a.writes() == 2);
}

TEST_CASE("project.autosave: never writes the user's project and reads back to the edited model", "[project]")
{
    Env env("autosave-safe");
    REQUIRE(env.store().create("Mine").ok());
    REQUIRE(env.save(SaveKind::Save, {}).ok());
    const auto bundle = env.store().file();
    const auto before = tree(bundle);
    const auto beforeTime = bundle.getChildFile("project.json").getLastModificationTime();
    // A rich edit: a random project's content as the "unsaved" state.
    auto rich = storetest::randomProject(11, 0.2, true);
    env.bus.execute(std::make_unique<st::PoolEdit>("POOL", rich->sampleRefs.entries, std::vector<st::PoolEdit::TrackKey> {}));
    setTempo(env.bus, 133.5);
    for (int i = 0; i < 5; ++i)
    {
        setTempo(env.bus, 133.5 + i);
        REQUIRE(env.session->autosave().writeNow());
        env.workers.drain();
    }
    CHECK(tree(bundle) == before); // PRJ-031
    CHECK(bundle.getChildFile("project.json").getLastModificationTime() == beforeTime);
    CHECK(env.store().dirty());
    for (const auto& e : juce::RangedDirectoryIterator(env.system().getChildFile("autosave"), false, "*", juce::File::findFiles))
        CHECK(e.getFile().getFileName() == "Mine.json"); // one file, no temp leftovers

    auto back = std::make_unique<m::Project>();
    st::ImportReport report;
    REQUIRE(st::Autosave::readInto(env.session->autosave().file(), *back, report).ok());
    CHECK(*back == env.project());
    const auto header = st::Autosave::readHeader(env.session->autosave().file());
    REQUIRE(header.has_value());
    CHECK(header->project == bundle);
    CHECK(header->name == "Mine");
}

TEST_CASE("project.autosave: an unsaved new project is autosaved under its name", "[project]")
{
    Env env("autosave-untitled");
    env.startup();
    setTempo(env.bus, 99.0);
    REQUIRE(env.session->autosave().writeNow());
    env.workers.drain();
    CHECK(env.system().getChildFile("autosave/Untitled.json").existsAsFile());
}

TEST_CASE("project.device: losing the audio device pauses, autosaves at once and resumes on reconnect", "[project]")
{
    fakes::FakeEngineBridge bridge;
    auto disk = std::make_shared<libtest::TempDir>("device");
    services::library::Workers workers {1};
    core::CommandBus bus;
    st::ProjectSession session(bus, workers, &bridge, nullptr,
                               {.dataDir = disk->dir().getChildFile("data"), .useOsTrash = false});
    session.startup([](const st::StartupResult&) {});
    workers.drain();
    REQUIRE(session.store().create("Gig").ok());
    setTempo(bus, 128.0); // unsaved
    bridge.playheadFeed.playing = true;
    bridge.playheadFeed.ppq = 37.25;
    bridge.posted.clear();
    std::vector<st::DeviceState> states;
    session.onDeviceState = [&](st::DeviceState s) { states.push_back(s); };

    session.onAudioDeviceLost();
    session.onAudioDeviceLost(); // reported twice by the io layer: once is enough
    workers.drain();
    CHECK(session.deviceState() == st::DeviceState::Lost);
    CHECK(session.autosave().writes() == 1);
    CHECK(session.autosave().file().existsAsFile());
    REQUIRE(bridge.posted.size() == 1);
    CHECK(std::get<engine::TransportCmd>(bridge.posted[0]).kind == engine::TransportCmd::Stop);
    CHECK(std::get<engine::TransportCmd>(bridge.posted[0]).value == 0.0); // pause: position kept
    session.frame(1e9); // no autosave tick logic breaks while lost
    workers.drain();

    bridge.posted.clear();
    session.onAudioDeviceRestored();
    CHECK(session.deviceState() == st::DeviceState::Running);
    REQUIRE(bridge.posted.size() == 2);
    CHECK(std::get<engine::TransportCmd>(bridge.posted[0]).kind == engine::TransportCmd::Locate);
    CHECK(std::get<engine::TransportCmd>(bridge.posted[0]).value == 37.25);
    CHECK(std::get<engine::TransportCmd>(bridge.posted[1]).kind == engine::TransportCmd::Continue);
    CHECK(states == std::vector<st::DeviceState> {st::DeviceState::Lost, st::DeviceState::Running});
    session.shutdown();
}

TEST_CASE("project.power: a power-off shutdown autosaves unsaved changes, a plain quit does not", "[project]")
{
    for (const bool powerOff : {false, true})
    {
        INFO("powerOff " << powerOff);
        fakes::FakeEngineBridge bridge;
        auto disk = std::make_shared<libtest::TempDir>("power");
        services::library::Workers workers {1};
        core::CommandBus bus;
        st::ProjectSession session(bus, workers, &bridge, nullptr,
                                   {.dataDir = disk->dir().getChildFile("data"), .useOsTrash = false});
        session.startup([](const st::StartupResult&) {});
        workers.drain();
        REQUIRE(session.store().create("Gig").ok());
        setTempo(bus, 128.0); // unsaved, younger than the autosave interval
        session.shutdown(powerOff); // the write completes inside: shutdown drains the workers
        CHECK(session.autosave().writes() == (powerOff ? 1 : 0));
        CHECK(session.autosave().file().existsAsFile() == powerOff);
    }
}

TEST_CASE("project.device: an audio drop-out stops the real engine cleanly and the project is autosaved", "[project]")
{
    auto engine = engine::createEngine();
    fakes::FakeAudioDevice device;
    std::string info;
    REQUIRE(device.start({}, *engine, info));
    REQUIRE(device.runPeriods(20));
    const auto blocks = engine->blocksRendered();
    CHECK(blocks > 0);

    auto disk = std::make_shared<libtest::TempDir>("dropout");
    services::library::Workers workers {1};
    core::CommandBus bus;
    st::ProjectSession session(bus, workers, &engine->bridge(), nullptr,
                               {.dataDir = disk->dir().getChildFile("data"), .useOsTrash = false});
    session.startup([](const st::StartupResult&) {});
    workers.drain();
    REQUIRE(session.store().create("Live").ok());
    setTempo(bus, 140.0);

    device.stop(); // the codec went away (io detects it; the session keeps the data safe)
    session.onAudioDeviceLost();
    CHECK_FALSE(device.runPeriods(5)); // nothing drives the engine
    engine->bridge().retirePending();
    workers.drain();
    CHECK(session.autosave().writes() == 1);
    CHECK(engine->blocksRendered() == blocks);

    REQUIRE(device.start({}, *engine, info)); // replugged
    session.onAudioDeviceRestored();
    REQUIRE(device.runPeriods(20));
    CHECK(engine->blocksRendered() > blocks);
    bool finite = true;
    for (float v : device.mainLeft())
        finite = finite && std::isfinite(v);
    CHECK(finite);
    session.shutdown();
}
