// No clicks on parameter changes (NFR-031: "all ramps as specified"; audio-engine.md fader 100-sample ramp) and
// unity paths bit-transparent (NFR-030), checked with step responses: the ParamSmoother itself, then end to end through
// the engine (track 1 "track.volume" on the test tone).
#include "tests/unit/engine/EngineTestUtil.h"

#include "engine/stages/ParamSmoother.h"
#include "engine/stages/TestToneStage.h"

#include "core/util/GainLaw.h"
#include "core/util/Realtime.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstring>

using engine::stages::ParamSmoother;

TEST_CASE("smoothing: step response is a linear ramp that lands exactly on the target", "[engine][smoothing]")
{
    for (const int ramp : {32, 33, 63, 64, 100, 128, 1000})
    {
        INFO("ramp " << ramp);
        ParamSmoother s;
        s.reset(1.0f);
        s.setTarget(0.0f, ramp);
        std::vector<float> v(static_cast<std::size_t>(ramp + 70));
        for (std::size_t done = 0; done < v.size(); done += 64) // block-wise, as the engine calls it
            s.fill(v.data() + done, static_cast<int>(std::min<std::size_t>(64, v.size() - done)));
        float prev = 1.0f;
        for (int i = 0; i < ramp; ++i)
        {
            const float expected = 1.0f - static_cast<float>(i + 1) / static_cast<float>(ramp);
            CHECK(std::fabs(v[static_cast<std::size_t>(i)] - expected) < 1e-5f);
            CHECK(v[static_cast<std::size_t>(i)] <= prev); // monotonic, no overshoot
            prev = v[static_cast<std::size_t>(i)];
        }
        for (std::size_t i = static_cast<std::size_t>(ramp - 1); i < v.size(); ++i)
            CHECK(v[i] == 0.0f); // exact: silence is silence
        CHECK(s.isConstant());
    }
}

TEST_CASE("smoothing: a new target mid-ramp continues from the value reached", "[engine][smoothing]")
{
    ParamSmoother s;
    s.reset(0.0f);
    s.setTarget(1.0f, 100);
    float a[64], b[64];
    s.fill(a, 50);
    const float reached = s.current();
    CHECK(reached == Catch::Approx(0.5f).margin(1e-5));
    s.setTarget(0.0f, 40);
    s.fill(b, 64);
    CHECK(std::fabs(b[0] - reached) <= reached / 40.0f + 1e-6f); // one ramp step, no jump
    CHECK(b[39] == 0.0f);
    // rampSamples 0…31 never step faster than the floor ParamSmoother::kMinRamp
    for (const int ramp : {0, 1, 10, 31})
    {
        s.reset(1.0f);
        s.setTarget(0.0f, ramp);
        s.fill(a, 64);
        CHECK(a[0] == Catch::Approx(1.0f - 1.0f / ParamSmoother::kMinRamp).margin(1e-6));
        CHECK(a[ParamSmoother::kMinRamp - 2] > 0.0f);
        CHECK(a[ParamSmoother::kMinRamp - 1] == 0.0f);
    }
    // skip() (a silent stage) tracks the ramp too
    s.reset(0.0f);
    s.setTarget(1.0f, 128);
    s.skip(64);
    CHECK(s.current() == Catch::Approx(0.5f).margin(1e-5));
    s.skip(64);
    CHECK(s.current() == 1.0f);
}

TEST_CASE("smoothing: engine volume steps never click (NFR-031)", "[engine][smoothing][realtime]")
{
    // The steady tone's largest sample-to-sample step: 2π · 440 / 44100 · 0.25 ≈ 0.0157. A parameter change must not
    // exceed it noticeably; an unsmoothed step 100 → 0 would jump by up to 0.25.
    const float steadyStep = 2.0f * 3.14159265f * 440.0f / 44100.0f * 0.25f;
    struct Case
    {
        float from, to;
        std::uint16_t ramp;
    };
    for (const Case c : {Case {100, 0, 0}, Case {100, 0, 64}, Case {100, 0, 100}, Case {0, 100, 0}, Case {100, 127, 100},
                         Case {127, 10, 1}, Case {10, 127, 5}})
    {
        INFO("volume " << c.from << " -> " << c.to << ", ramp " << c.ramp);
        auto e = enginetest::make();
        auto& b = e->bridge();
        REQUIRE(b.post(engine::ParamChange {enginetest::volumeId(), 0, c.from, 0}));
        enginetest::render(*e, 64 * 20, 64); // settle at `from`
        REQUIRE(b.post(engine::ParamChange {enginetest::volumeId(), 0, c.to, c.ramp}));
        const auto out = enginetest::render(*e, 64 * 10, 100); // the change lands at the next block start
        const float levelFrom = 0.25f * core::GainLaw::gain(static_cast<int>(c.from));
        const float levelTo = 0.25f * core::GainLaw::gain(static_cast<int>(c.to));
        const float bound = 2.0f * 3.14159265f * 440.0f / 44100.0f * std::max(levelFrom, levelTo);
        const int ramp = std::max<int>(c.ramp, ParamSmoother::kMinRamp);
        const float rampStep = std::fabs(levelTo - levelFrom) / static_cast<float>(ramp);
        // largest step = the sine's own slope at the larger level + one ramp increment (an unsmoothed change would
        // add the whole |levelTo − levelFrom|)
        CHECK(enginetest::maxStep(out.l) <= bound + rampStep + 1e-4f);
        CHECK(rampStep < std::fabs(levelTo - levelFrom) / 30.0f);
        CHECK(bound <= 2.01f * steadyStep);
        // after the ramp the level is the target's (the last 3 blocks)
        CHECK(enginetest::peak(out.l, out.l.size() - 192) == Catch::Approx(levelTo).margin(0.002));
    }
#if APP_RT_CHECKS
    CHECK(core::rt::violationCount() == 0);
#endif
}

TEST_CASE("smoothing: a unity track reaches the master bit-exactly (NFR-030)", "[engine][smoothing]")
{
    // The empty stages (inserts, strip, master FX, master fader, output volume) are unity: Main equals the test-tone
    // stage's own output sample for sample.
    engine::stages::TestToneStage tone;
    tone.prepare(44100.0);
    engine::stages::StereoBlock block {};
    std::vector<float> expected;
    for (int b = 0; b < 100; ++b)
    {
        REQUIRE(tone.process(block));
        expected.insert(expected.end(), block.l, block.l + 64);
    }
    auto e = enginetest::make();
    const auto out = enginetest::render(*e, 6400, 128);
    CHECK(std::memcmp(out.l.data(), expected.data(), sizeof(float) * 6400) == 0);
    CHECK(std::memcmp(out.r.data(), expected.data(), sizeof(float) * 6400) == 0);
}
