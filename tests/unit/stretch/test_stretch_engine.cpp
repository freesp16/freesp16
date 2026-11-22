// MASTER TEMPO through the whole engine (Engine::stretchBank(), the step 8a hunk): a sample installed by ObjectSwap
// with TIME STRETCH = MASTER TEMPO (playback.timeStretch 2), its BPM given to the bank, the transport tempo from
// transport.tempo: the pad hit plays stretched — twice as long at half the tempo, at the sample's pitch — and a
// backend change while it plays is installed at a block start without an allocation on the audio thread.
#include "engine/voice/VoiceSwaps.h"
#include "tests/unit/engine/EngineTestUtil.h"
#include "tests/unit/voice/VoiceHarness.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace
{
core::ParamId id(const char* name) { return core::builtinParams().idOf(name).value_or(core::ParamId {}); }

long lastAbove(const std::vector<float>& x, float level)
{
    for (std::size_t i = x.size(); i-- > 0;)
        if (std::fabs(x[i]) > level)
            return static_cast<long>(i);
    return -1;
}
} // namespace

TEST_CASE("stretch engine: a MASTER TEMPO pad hit follows the transport tempo at its own pitch", "[stretch][engine]")
{
    voicetest::TestBuffer buf(voicetest::sine(44100, 440.0, voicetest::kRate, 0.5f)); // 1 s at "120 BPM"
    {
        auto e = enginetest::make(false);
        core::audio::retain(buf.get());
        REQUIRE(e->bridge().post(engine::ObjectSwap {engine::voice::trackSampleSlot(0, 0), buf.get()}));
        REQUIRE(e->bridge().post(engine::ParamChange {id("playback.timeStretch"), 0, 2.0f, 0}));
        REQUIRE(e->bridge().post(engine::ParamChange {id("transport.tempo"), 0, 60.0f, 0}));
        e->stretchBank().setSampleTempo(0, 0, 120.0f);
        enginetest::render(*e, 1024, 256);
        REQUIRE(e->triggers().push({0, 127, 60, 0}));
        const auto out = enginetest::render(*e, 3 * 44100, 256);
        CHECK(enginetest::peak(out.l) > 0.1f);
        CHECK(std::fabs(voicetest::frequency(out.l, 4410, 80000) - 440.0) <= 1.0);
        const long end = lastAbove(out.l, 0.02f);
        INFO("end " << end);
        CHECK(std::fabs(end - 88200.0) <= 1800.0); // 2 s (the envelope's release adds a little)
        // A backend change: queued on this thread, installed by the audio thread at its next block.
        REQUIRE(e->stretchBank().select("pvoc"));
        REQUIRE(e->triggers().push({0, 127, 60, 0}));
        const auto again = enginetest::render(*e, 8192, 256);
        CHECK(e->stretchBank().installedName() == "pvoc");
        CHECK(e->trackSource(0).voice().stretching());
        CHECK(std::fabs(voicetest::frequency(again.l, 2048, 8192) - 440.0) <= 2.0);
        e->stretchBank().collect();
        REQUIRE(e->bridge().post(engine::ObjectSwap {engine::voice::trackSampleSlot(0, 0), nullptr}));
        enginetest::render(*e, 16384, 256);
        e->bridge().retirePending();
    }
    CHECK(buf.refs() == 0);
}
