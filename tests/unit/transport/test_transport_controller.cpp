// ui::input::transport::TransportController: PLAY / STOP / REC keys → TransportCmd, the
// pre-count from the REC arm, TAP and NUDGE → transport.* parameters, following the engine's playhead, the shell's
// UiState; and the transport.* ParamSpecs (core/params/catalogue/transport.cpp).
#include "ui/input/transport/TransportController.h"

#include "core/params/ParamRegistry.h"
#include "tests/fakes/FakeEngineBridge.h"
#include "ui/shell/ParamAccess.h"
#include "ui/shell/UiState.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <variant>

using engine::TransportCmd;
using ui::input::transport::State;
using ui::input::transport::TransportController;

namespace
{
const TransportCmd* lastCmd(const fakes::FakeEngineBridge& b)
{
    for (auto it = b.posted.rbegin(); it != b.posted.rend(); ++it)
        if (const auto* c = std::get_if<TransportCmd>(&*it))
            return c;
    return nullptr;
}
const engine::ParamChange* lastParam(const fakes::FakeEngineBridge& b)
{
    for (auto it = b.posted.rbegin(); it != b.posted.rend(); ++it)
        if (const auto* c = std::get_if<engine::ParamChange>(&*it))
            return c;
    return nullptr;
}
bool is(const TransportCmd* c, TransportCmd::Kind k, double v)
{
    return c != nullptr && c->kind == k && c->value == v;
}
io::ControlEvent press(io::Control c, double t = 0) { return {io::ControlEvent::Kind::Press, c, 100, io::kSourcePanel, t}; }
io::ControlEvent release(io::Control c, double t = 0)
{
    return {io::ControlEvent::Kind::Release, c, 0, io::kSourcePanel, t};
}
} // namespace

TEST_CASE("transport.params: the transport.* ParamSpecs", "[transport]")
{
    const auto& r = core::builtinParams();
    const auto* tempo = r.find("transport.tempo");
    REQUIRE(tempo != nullptr);
    CHECK(tempo->minValue == 20.0f);
    CHECK(tempo->maxValue == 300.0f);
    CHECK(tempo->defaultValue == 120.0f);
    CHECK(tempo->coarseStep == 1.0f);
    CHECK(tempo->fineStep == 0.1f);
    CHECK(tempo->scope == core::ParamScope::Project);
    CHECK(tempo->featureFlag.empty()); // the real spec, not the interim stub
    char buf[16];
    tempo->formatValue(128.3f, buf, sizeof(buf));
    CHECK(std::string(buf) == "128.3");
    for (const char* id : {"transport.metronome", "transport.metronomeVolume", "transport.metronomeMonitor",
                           "transport.preCount", "transport.nudge", "transport.nudgeAmount"})
    {
        const auto* s = r.find(id);
        INFO(id);
        REQUIRE(s != nullptr);
        CHECK(s->featureFlag.empty());
        CHECK(s->shortLabel.size() <= 8);
    }
    CHECK(r.find("transport.metronomeVolume")->defaultValue == 100.0f);
    CHECK(r.find("transport.preCount")->enumLabels.size() == 3);
    CHECK(r.find("transport.nudge")->minValue == -1.0f);
    CHECK(r.find("transport.nudgeAmount")->defaultValue == 3.0f);
}

TEST_CASE("transport.controller: PLAY toggles play/pause, STOP twice is the hard stop", "[transport]")
{
    fakes::FakeEngineBridge bridge;
    TransportController t(bridge);
    t.onPlay();
    CHECK(is(lastCmd(bridge), TransportCmd::Play, 0.0));
    CHECK(t.state() == State::Playing);
    t.onPlay();
    CHECK(is(lastCmd(bridge), TransportCmd::Stop, 0.0)); // pause
    CHECK(t.state() == State::Paused);
    t.onPlay();
    CHECK(is(lastCmd(bridge), TransportCmd::Continue, 0.0));
    CHECK(t.state() == State::Playing);
    t.onStop();
    CHECK(is(lastCmd(bridge), TransportCmd::Stop, 1.0)); // back to step 1, tails ring
    CHECK(t.state() == State::Stopped);
    t.onStop();
    CHECK(is(lastCmd(bridge), TransportCmd::Stop, 2.0)); // second STOP: silence everything
    // STOP from a pause is a stop, not a hard stop.
    t.onPlay();
    t.onPlay();
    t.onStop();
    CHECK(is(lastCmd(bridge), TransportCmd::Stop, 1.0));
    // The dispatcher's events.
    bridge.posted.clear();
    CHECK(t.handle(press(io::Control::Play)));
    CHECK(t.handle(release(io::Control::Play)));
    CHECK(t.handle(press(io::Control::Stop)));
    CHECK_FALSE(t.handle(press(io::Control::Pad1)));
    CHECK(bridge.posted.size() == 2);
}

TEST_CASE("transport.controller: REC armed + PLAY from stop runs the pre-count of transport.preCount", "[transport]")
{
    fakes::FakeEngineBridge bridge;
    ui::LocalParamValues values(core::builtinParams(), &bridge);
    TransportController t(bridge, &values);
    values.set(*core::builtinParams().find("transport.preCount"), 0, 2.0f);
    t.onPlay(); // not armed: no pre-count
    CHECK(is(lastCmd(bridge), TransportCmd::Play, 0.0));
    t.onStop();
    CHECK(t.handle(press(io::Control::Rec)));
    CHECK(t.recArmed());
    t.onPlay();
    CHECK(is(lastCmd(bridge), TransportCmd::Play, 2.0));
    CHECK(t.state() == State::PreCount);
    t.onPlay(); // PLAY during the count-in cancels it
    CHECK(is(lastCmd(bridge), TransportCmd::Stop, 1.0));
    t.onRec();
    CHECK_FALSE(t.recArmed());
}

TEST_CASE("transport.controller: TAP sets transport.tempo, NUDGE posts ±1 while held", "[transport]")
{
    fakes::FakeEngineBridge bridge;
    ui::LocalParamValues values(core::builtinParams(), &bridge);
    TransportController t(bridge, &values);
    const auto* tempo = core::builtinParams().find("transport.tempo");
    t.handle(press(io::Control::ExtTap, 1000));
    t.handle(press(io::Control::ExtTap, 1500));
    CHECK(values.get(*tempo, 0) == 120.0f);
    t.onTap(2000 + 2500); // a new series after 2 s: no tempo yet
    t.onTap(4500 + 400);
    CHECK(values.get(*tempo, 0) == 150.0f);
    t.onTap(4900 + 300); // the average of 400 and 300 ms → 171.4 BPM (0.1 grid)
    CHECK(values.get(*tempo, 0) == 171.4f);

    const auto nudgeId = core::builtinParams().idOf("transport.nudge");
    t.handle(press(io::Control::ExtNudgeUp));
    REQUIRE(lastParam(bridge) != nullptr);
    CHECK(lastParam(bridge)->param == *nudgeId);
    CHECK(lastParam(bridge)->value == 1.0f);
    CHECK(t.nudge() == 1);
    t.handle(release(io::Control::ExtNudgeUp));
    CHECK(lastParam(bridge)->value == 0.0f);
    t.handle(press(io::Control::ExtNudgeDown));
    CHECK(lastParam(bridge)->value == -1.0f);
    t.handle(release(io::Control::ExtNudgeUp)); // the other side's release keeps the held one
    CHECK(t.nudge() == -1);
    t.handle(release(io::Control::ExtNudgeDown));
    CHECK(t.nudge() == 0);

    // Without a ParamAccess: SetTempo straight to the engine.
    fakes::FakeEngineBridge direct;
    TransportController d(direct);
    d.onTap(0);
    d.onTap(500);
    CHECK(is(lastCmd(direct), TransportCmd::SetTempo, 120.0));
}

TEST_CASE("transport.controller: follows the engine's playhead and writes the shell's state", "[transport]")
{
    fakes::FakeEngineBridge bridge;
    TransportController t(bridge);
    engine::PlayheadFeed feed {};
    feed.blockIndex = 100;
    t.sync(feed);
    t.onPlay();
    feed.blockIndex = 110; // too early: our Play may not be in the snapshot yet
    t.sync(feed);
    CHECK(t.state() == State::Playing);
    feed.blockIndex = 200;
    feed.playing = true;
    feed.ppq = 16.5;
    feed.bpm = 128.0;
    t.sync(feed);
    ui::UiState s;
    t.publish(s);
    CHECK(s.playing);
    CHECK(std::string(s.position.view()) == "005:01:03");
    CHECK(std::string(s.bpm.view()) == "128.0");
    t.publish(s, 16.0); // pattern-relative
    CHECK(std::string(s.position.view()) == "001:01:03");
    // Stopped by someone else (a project load posts Stop(1)): the engine reports ppq 0, not playing.
    feed.blockIndex = 300;
    feed.playing = false;
    feed.ppq = 0.0;
    t.sync(feed);
    CHECK(t.state() == State::Stopped);
    t.publish(s);
    CHECK_FALSE(s.playing);
    CHECK(std::string(s.position.view()) == "001:01:01");
    // Started elsewhere, then paused elsewhere (position kept).
    feed.blockIndex = 400;
    feed.playing = true;
    feed.ppq = 3.0;
    t.sync(feed);
    CHECK(t.state() == State::Playing);
    feed.blockIndex = 500;
    feed.playing = false;
    t.sync(feed);
    CHECK(t.state() == State::Paused);
    // Pre-count shown as the bars left.
    feed.blockIndex = 600;
    feed.preCount = true;
    feed.ppq = -7.0;
    t.sync(feed);
    CHECK(t.state() == State::PreCount);
    t.onRec();
    t.publish(s);
    CHECK(s.recArmed);
    CHECK(std::string(s.position.view()) == "-02:02:01");
}
