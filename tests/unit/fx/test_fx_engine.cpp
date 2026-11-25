// The FX slot bank inside engine::Engine: ParamTarget::Fx routing, the mixer hooks per block, the
// effect panel feed through EngineBridge::fxMeters, the metered slot, and the second-STOP edge of the transport.
#include "engine/voice/VoiceSwaps.h"
#include "tests/unit/engine/EngineTestUtil.h"
#include "tests/unit/voice/VoiceHarness.h"

#include "engine/fx/EffectRegistryOrder.h"
#include "engine/fx/FxSlotState.h"

#include <catch2/catch_test_macros.hpp>

namespace
{
using engine::fx::SlotKind;

float typeValue(SlotKind kind, std::string_view id)
{
    const auto list = engine::fx::engineTypes(engine::fx::EffectRegistry::builtin(), kind);
    const int i = engine::fx::typeIndexOf(list, id);
    REQUIRE(i >= 0);
    return static_cast<float>(i + 1);
}

bool post(engine::Engine& e, std::string_view id, float v, std::uint16_t scope)
{
    const auto pid = core::builtinParams().idOf(id);
    REQUIRE(pid.has_value());
    return e.bridge().post(engine::ParamChange {*pid, scope, v, 0});
}
} // namespace

TEST_CASE("fx.engine: the bank is routed and the Master FX runs as the mixer's hook", "[fx][engine]")
{
    engine::EngineOptions o; // the test tone on track 1 is the signal (−12 dBFS)
    auto e = std::make_unique<engine::Engine>(enginetest::fullSetup(), o);
    e->prepare(44100.0, 512);
    CHECK(e->fxBank().routes().routed() > 0);
    const float before = enginetest::peak(enginetest::render(*e, 4096).l, 2048);
    REQUIRE(before > 0.1f);

    // GAIN TEST on the Master slot at gain index 0 (−∞): the main bus goes silent after the activation fade.
    REQUIRE(post(*e, "master.gainTest.gain", 0.0f, 0));
    REQUIRE(post(*e, "master.type", typeValue(SlotKind::Master, "gainTest"), 0));
    REQUIRE(post(*e, "fx.meteredSlot", static_cast<float>(engine::fx::kMasterSlot), 0xFFFF));
    const auto muted = enginetest::render(*e, 8192);
    CHECK(enginetest::peak(muted.l, 6144) < 1e-4f);
    CHECK_FALSE(e->fxBank().masterIdle());

    const auto* feed = e->bridge().fxMeters();
    REQUIRE(feed != nullptr);
    CHECK(feed->meteredSlot == engine::fx::kMasterSlot);
    CHECK(feed->current[engine::fx::kMasterSlot] == static_cast<int>(typeValue(SlotKind::Master, "gainTest")) - 1);
    CHECK(((feed->runningMask >> engine::fx::kMasterSlot) & 1u) == 1u);
    CHECK(feed->inPeak > 0.1f);  // the tone reaches the slot
    CHECK(feed->outPeak < 1e-3f); // and leaves it silenced

    // Off again (type 0): the master passes the tone unchanged.
    REQUIRE(post(*e, "master.type", 0.0f, 0));
    const auto back = enginetest::render(*e, 16384);
    CHECK(enginetest::peak(back.l, 12288) > 0.9f * before);
}

TEST_CASE("fx.engine: a track insert runs on its track only, in the current scene (FX-004)", "[fx][engine]")
{
    engine::EngineOptions o;
    auto e = std::make_unique<engine::Engine>(enginetest::fullSetup(), o);
    e->prepare(44100.0, 512);
    const float before = enginetest::peak(enginetest::render(*e, 4096).l, 2048);
    // Scene 2's FX1 on track 1 does not touch scene 1.
    REQUIRE(post(*e, "fx1.gainTest.gain", 0.0f, (1 << 4) | 0));
    REQUIRE(post(*e, "fx1.type", typeValue(SlotKind::Insert, "gainTest"), (1 << 4) | 0));
    CHECK(enginetest::peak(enginetest::render(*e, 8192).l, 6144) > 0.9f * before);
    CHECK(e->fxBank().insertIdle(0));
    // Scene 1, track 1 (the tone's track): silenced; track 2's slot stays idle.
    REQUIRE(post(*e, "fx1.gainTest.gain", 0.0f, 0));
    REQUIRE(post(*e, "fx1.type", typeValue(SlotKind::Insert, "gainTest"), 0));
    CHECK(enginetest::peak(enginetest::render(*e, 8192).l, 6144) < 1e-4f);
    CHECK_FALSE(e->fxBank().insertIdle(0));
    CHECK(e->fxBank().insertIdle(1));
    // An id the bank does not route is refused at post (unbound), not silently dropped.
    const auto other = core::builtinParams().idOf("fx.restartLfoOnPlay");
    REQUIRE(other.has_value());
    CHECK_FALSE(e->bridge().post(engine::ParamChange {*other, 0, 1.0f, 0}));
}

TEST_CASE("fx.engine: without set-up ids the bank is empty and every slot idle", "[fx][engine]")
{
    auto e = enginetest::make();
    CHECK(e->fxBank().routes().routed() == 0);
    const auto s = enginetest::render(*e, 2048);
    CHECK(enginetest::peak(s.l) > 0.1f);
    CHECK(e->fxBank().masterIdle());
    CHECK(e->fxBank().sendIdle());
}

TEST_CASE("fx.engine: STOP while stopped is the second STOP (FX-036)", "[fx][engine]")
{
    engine::stages::TransportStage t;
    t.prepare(44100.0);
    engine::BlockContext ctx {};
    engine::stages::BlockBuffers b {};
    t.command({engine::TransportCmd::Play, 0});
    t.process(ctx, b);
    // Stop(1) = STOP (Stop(0) is a pause; ui::input::transport::TransportController posts 1).
    t.command({engine::TransportCmd::Stop, 1.0});
    t.process(ctx, b);
    CHECK(ctx.stoppedThisBlock);
    CHECK_FALSE(t.secondStopThisBlock()); // the first STOP only stops
    t.command({engine::TransportCmd::Stop, 1.0});
    t.process(ctx, b);
    CHECK(t.secondStopThisBlock());
    t.process(ctx, b);
    CHECK_FALSE(t.secondStopThisBlock()); // one block only
    t.command({engine::TransportCmd::Pause, 0});
    t.process(ctx, b);
    CHECK_FALSE(t.secondStopThisBlock()); // PAUSE never clears the tails
}

TEST_CASE("fx.engine: a loop stops feeding master reverb on STOP and hard STOP clears its tail",
          "[fx][engine][stop-audio]")
{
    voicetest::TestBuffer buffer(voicetest::sine(4096, 220.0, 44100.0, 0.1f));
    engine::EngineOptions opts;
    opts.testTone = false;
    auto e = std::make_unique<engine::Engine>(enginetest::fullSetup(), opts);
    e->prepare(44100.0, 256);
    core::audio::retain(buffer.get());
    REQUIRE(e->bridge().post(engine::ObjectSwap{engine::voice::trackSampleSlot(0, 0), buffer.get()}));
    REQUIRE(post(*e, "playback.loop", 1.0f, 0));
    REQUIRE(post(*e, "master.type", typeValue(SlotKind::Master, "reverb"), 0));
    REQUIRE(post(*e, "master.reverb.mix", 0.5f, 0));
    REQUIRE(e->bridge().post(engine::TransportCmd{engine::TransportCmd::Play, 0.0}));
    REQUIRE(e->bridge().post(engine::TriggerNow{0, 127, 60, 0}));
    REQUIRE(enginetest::peak(enginetest::render(*e, 22050).l, 16384) > 0.01f);

    REQUIRE(e->bridge().post(engine::TransportCmd{engine::TransportCmd::Stop, 1.0}));
    CHECK(enginetest::peak(enginetest::render(*e, 4096).l, 2048) > 0.0001f);
    CHECK_FALSE(e->trackSource(0).voice().mainActive());
    REQUIRE(e->bridge().post(engine::TransportCmd{engine::TransportCmd::Stop, 2.0}));
    CHECK(enginetest::peak(enginetest::render(*e, 44100).l, 2048) < 0.00001f);
}
