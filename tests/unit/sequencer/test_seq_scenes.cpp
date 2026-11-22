// A scene switch reaches the track sources, the trigger router and the FX slots (resident scenes, an index change:
// ScenePatternSwitcher.h): scene 2's sample on track 1 plays after the switch, scene 1's before; scene 2's FX1 runs
// only once scene 2 is current. Engine::switchScene applies at the start of the boundary block (our choice), the mixer
// on the frame.
#include "tests/unit/sequencer/SeqRig.h"

#include "engine/fx/EffectRegistryOrder.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

using namespace seqtest;

namespace
{
voicetest::TestBuffer& level(int which)
{
    static voicetest::TestBuffer a(std::vector<float>(400000, 0.1f)), b(std::vector<float>(400000, 0.4f));
    return which == 0 ? a : b;
}

bool installSample(Rig& rig, int scene, int track, int which)
{
    core::audio::retain(level(which).get());
    return rig.e->bridge().post(engine::ObjectSwap {engine::voice::trackSampleSlot(scene, track), level(which).get()});
}

float peak(const Rig& rig, long from, long to)
{
    float p = 0.0f;
    for (long i = from; i < to && i < static_cast<long>(rig.mainL.size()); ++i)
        p = std::max(p, std::fabs(rig.mainL[static_cast<std::size_t>(i)]));
    return p;
}

std::unique_ptr<core::model::Pattern> everyFourth(int track)
{
    auto p = pattern(16);
    for (int s = 0; s < 16; s += 4)
        put(*p, s, track);
    return p;
}
} // namespace

TEST_CASE("seq.scenes: after a scene switch track 1 plays scene 2's sample, before it scene 1's",
          "[seq][engine][scene]")
{
    Rig rig;
    REQUIRE(installSample(rig, 0, 0, 0)); // scene 1: 0.1
    REQUIRE(installSample(rig, 1, 0, 1)); // scene 2: 0.4
    REQUIRE(rig.install(*everyFourth(0), 0, 0));
    REQUIRE(rig.install(*everyFourth(0), 1, 0));
    REQUIRE(rig.play());
    const double st = stepFrames();
    rig.until(static_cast<long>(2 * st));
    REQUIRE(rig.queue(1, 0)); // PATTERN unit: at the end of the pass, PPQ 4.0 = frame 88,200
    rig.until(88200 + static_cast<long>(6 * st));
    const float before = peak(rig, static_cast<long>(13 * st), 88200 - 200);
    const float after = peak(rig, 88200 + 400, 88200 + static_cast<long>(3.5 * st));
    INFO("before " << before << " after " << after);
    REQUIRE(before > 0.01f);
    CHECK(after / before == Catch::Approx(4.0f).epsilon(0.02)); // same strip, 4× the sample level
    CHECK(rig.e->triggerRouter().scene() == 1);
    // The new scene's hit lands on the boundary frame.
    const auto hits = rig.sequenced(0);
    CHECK(std::any_of(hits.begin(), hits.end(), [](const Hit& h) { return h.frame == 88200; }));
}

TEST_CASE("seq.scenes: scene 2's insert FX is resident and runs only while scene 2 is current", "[seq][engine][scene][fx]")
{
    Rig rig(Rig::Full {});
    REQUIRE(installSample(rig, 0, 0, 0));
    REQUIRE(installSample(rig, 1, 0, 0));
    const auto list = engine::fx::engineTypes(engine::fx::EffectRegistry::builtin(), engine::fx::SlotKind::Insert);
    const int type = engine::fx::typeIndexOf(list, "gainTest");
    REQUIRE(type >= 0);
    REQUIRE(rig.param("fx1.gainTest.gain", 0.0f, 0x10)); // scene 2, track 1: silence
    REQUIRE(rig.param("fx1.type", static_cast<float>(type + 1), 0x10));
    REQUIRE(rig.install(*everyFourth(0), 0, 0));
    REQUIRE(rig.install(*everyFourth(0), 1, 0));
    REQUIRE(rig.play());
    const double st = stepFrames();
    rig.until(static_cast<long>(2 * st));
    CHECK(rig.e->fxBank().insertIdle(0)); // scene 1 has no FX
    REQUIRE(rig.queue(1, 0));
    rig.until(88200 + static_cast<long>(14 * st));
    CHECK(peak(rig, static_cast<long>(12 * st), 88200 - 200) > 0.01f);
    CHECK_FALSE(rig.e->fxBank().insertIdle(0));
    CHECK(peak(rig, 88200 + static_cast<long>(2 * st), 88200 + static_cast<long>(14 * st)) < 1e-4f);
    REQUIRE(rig.queue(0, 0)); // back to scene 1 at the next pass end: FX off again, the sample sounds
    rig.until(2 * 88200 + static_cast<long>(6 * st));
    CHECK(rig.e->triggerRouter().scene() == 0);
    CHECK(peak(rig, 2 * 88200 + static_cast<long>(2 * st), 2 * 88200 + static_cast<long>(6 * st)) > 0.01f);
}
