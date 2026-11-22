// The ribbon inside engine::Engine: ribbon.* through the bridge, the strip stream through the TriggerSink
// (the device's reader-thread path), REPEAT hits through the trigger router on the running transport — and, when
// the engine has the LFO ModMatrix, PITCH end to end on MAIN (± 1 cent) through the matrix and the voice.
#include "tests/unit/engine/EngineTestUtil.h"
#include "tests/unit/voice/VoiceHarness.h"

#include "core/params/ParamRegistry.h"
#include "engine/voice/VoiceSwaps.h"
#include "io/controllers/StripSource.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

namespace
{
core::ParamId id(const char* name) { return core::builtinParams().idOf(name).value_or(core::ParamId {}); }

// One 64-frame block (one device period of 64 frames).
void block(engine::Engine& e)
{
    static std::vector<float> l(engine::kBlockFrames), r(engine::kBlockFrames);
    float* const out[2] = {l.data(), r.data()};
    engine::OfflineDriver d;
    d.periodFrames = engine::kBlockFrames;
    d.run(e, engine::kBlockFrames, out);
}

std::vector<long> repeatFrames(engine::Engine& e, long blocks, long& at)
{
    std::vector<long> hits;
    for (long n = 0; n < blocks; ++n, ++at)
    {
        block(e);
        for (const auto& ev : e.triggerRouter().blockEvents())
            if (ev.source == engine::trigger::TriggerSource::Repeat)
                hits.push_back(at * engine::kBlockFrames + ev.frame);
    }
    return hits;
}

// PITCH on MAIN through the LFO ModMatrix: only when Engine has modMatrix() (otherwise the discarded branch is not
// instantiated; everything but the matrix accessor is compiled here regardless).
// `buf` must outlive the engine: the engine's swap table still holds it and releases it when it is destroyed.
template <class E>
bool pitchEndToEnd(E& e, voicetest::TestBuffer& buf)
{
    if constexpr (requires { e.modMatrix(); })
    {
        core::audio::retain(buf.get());
        REQUIRE(e.bridge().post(engine::ObjectSwap {engine::voice::trackSampleSlot(0, 0), buf.get()}));
        enginetest::render(e, 1024, 256);
        REQUIRE(e.triggers().push({0, 127, 60, 0})); // pad 1 held
        REQUIRE(e.triggers().push(io::stripTrigger(127, true)));
        const auto bent = enginetest::render(e, 44100, 256);
        const double f = voicetest::frequency(bent.l, 4410, 44100);
        CHECK(std::fabs(1200.0 * std::log2(f / (440.0 * std::pow(2.0, 200.0 / 1200.0)))) <= 1.0); // +200 cents ± 1
        CHECK(e.modMatrix().contribution(0, id("playback.pitch"), engine::modulation::ModSource::Ribbon) ==
              Catch::Approx(engine::modulation::pitchOffset01(200.0f)));
        REQUIRE(e.triggers().push(io::stripTrigger(127, false))); // springs back (RIB-002)
        const auto back = enginetest::render(e, 22050, 256);
        CHECK(std::fabs(1200.0 * std::log2(voicetest::frequency(back.l, 4410, 22050) / 440.0)) <= 1.0);
        return true;
    }
    else
    {
        (void)e;
        (void)buf;
        return false;
    }
}
} // namespace

TEST_CASE("ribbon.engine: ribbon.* are bound; the strip stream arrives through the TriggerSink", "[ribbon][engine]")
{
    auto e = enginetest::make(false);
    auto& bridge = e->bridge();
    for (const char* name : {"ribbon.mode", "ribbon.hold", "ribbon.pitchTop", "ribbon.pitchBottom", "ribbon.userTarget",
                             "ribbon.userTop", "ribbon.userBottom", "ribbon.applyTo"})
    {
        INFO(name);
        CHECK(bridge.post(engine::ParamChange {id(name), 0, 0.0f, 0}));
    }
    REQUIRE(bridge.post(engine::ParamChange {id("ribbon.mode"), 0, 3.0f, 0}));
    REQUIRE(e->triggers().push(io::stripTrigger(96, true))); // the reader thread's path (UI-118)
    block(*e);
    CHECK(e->ribbon().mode() == engine::ribbon::Mode::User2);
    CHECK(e->ribbon().contact());
    CHECK(e->ribbon().position() == 96);
    CHECK(e->ribbon().ledMask() == core::ribbon::ledMask(engine::ribbon::Mode::User2, 96, true));
    CHECK(e->triggersReceived() == 0); // never a trigger
    REQUIRE(bridge.post(io::stripTrigger(96, false))); // the message thread's path (keyboard, script)
    block(*e);
    CHECK_FALSE(e->ribbon().contact());
}

TEST_CASE("ribbon.engine: REPEAT hits a held pad on the 1/8 grid of the running transport, then stops",
          "[ribbon][engine]")
{
    auto e = enginetest::make(false);
    REQUIRE(e->bridge().post(engine::ParamChange {id("ribbon.mode"), 0, 1.0f, 0}));
    REQUIRE(e->bridge().post(engine::TransportCmd {engine::TransportCmd::Play, 0.0}));
    long at = 0;
    repeatFrames(*e, 10, at);
    REQUIRE(e->triggers().push({4, 100, 60, 4}));            // pad 5 held (PLAY mode: track 5)
    REQUIRE(e->triggers().push(io::stripTrigger(40, true))); // 1/8 zone
    const auto hits = repeatFrames(*e, 2000, at);            // ≈ 2.9 s at 120 BPM
    CHECK(e->ribbon().heldTracks() == 1u << 4);
    REQUIRE(hits.size() >= 10);
    for (std::size_t i = 1; i < hits.size(); ++i)
        CHECK(std::labs(hits[i] - hits[i - 1] - 11025) <= 1); // 1/8 at 120 BPM = 11025 frames
    // on the grid: the transport started at the Play block's frame 0 (block 0 here), PPQ 0
    for (const long h : hits)
        CHECK(std::labs(h - 11025 * std::lround(static_cast<double>(h) / 11025.0)) <= 1);
    REQUIRE(e->triggers().push(io::stripTrigger(40, false)));
    CHECK(repeatFrames(*e, 1000, at).empty());
    CHECK(e->triggerRouter().dropped() == 0);
}

TEST_CASE("ribbon.wiring: PITCH reaches MAIN through the LFO ModMatrix (+200 cents ± 1)", "[ribbon][engine]")
{
    voicetest::TestBuffer buf(voicetest::sine(3 * 44100, 440.0, voicetest::kRate, 0.5f)); // declared first: outlives e
    auto e = enginetest::make(false);
    if (! pitchEndToEnd(*e, buf))
        SKIP("the engine has no ModMatrix in this build: the ribbon publishes into it when present");
}
