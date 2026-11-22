// TIME STRETCH planning (engine/voice/stretch/StretchMode.h): PLY-030 modes, PLY-031 sample BPM / bars / estimate,
// PLY-032 PITCH ignored in RESAMPLE, PLY-020 ±12 st in MASTER TEMPO, PLY-033 CC 14 bands.
#include "engine/voice/stretch/StretchMode.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace engine::voice::stretch;
using Catch::Matchers::WithinRel;

TEST_CASE("stretch mode: CC 14 in equal thirds", "[stretch]")
{
    for (int v = 0; v <= 127; ++v)
    {
        const StretchMode m = modeFromCc(v);
        INFO("cc " << v);
        CHECK(m == (v < 43 ? StretchMode::Off : (v < 86 ? StretchMode::Resample : StretchMode::MasterTempo)));
    }
    for (auto m : {StretchMode::Off, StretchMode::Resample, StretchMode::MasterTempo})
        CHECK(modeFromCc(ccFromMode(m)) == m);
}

TEST_CASE("stretch mode: OFF plays at the pitch rate", "[stretch]")
{
    const auto p = planFor(StretchMode::Off, 150.0, 120.0, 1200);
    CHECK_THAT(p.rate, WithinRel(2.0, 1e-12));
    CHECK(p.timeRatio == 1.0);
    CHECK(p.pitchScale == 1.0);
    CHECK_THAT(planFor(StretchMode::Off, 120.0, 0.0, 3000).rate, WithinRel(4.0, 1e-12)); // ±24 st
}

TEST_CASE("stretch mode: RESAMPLE follows the tempo, ignores PITCH", "[stretch]")
{
    const auto p = planFor(StretchMode::Resample, 150.0, 120.0, 700);
    CHECK_THAT(p.rate, WithinRel(1.25, 1e-12));
    CHECK(p.timeRatio == 1.0);
    CHECK(p.pitchScale == 1.0);
    CHECK(planFor(StretchMode::Resample, 150.0, 0.0, 0).rate == 1.0); // unknown sample BPM
    CHECK_THAT(planFor(StretchMode::Resample, 0.0, 60.0, 0).rate, WithinRel(2.0, 1e-12)); // project 0 → 120
}

TEST_CASE("stretch mode: MASTER TEMPO stretches, pitch within +-12 st", "[stretch]")
{
    const auto p = planFor(StretchMode::MasterTempo, 150.0, 120.0, 700);
    CHECK(p.rate == 1.0);
    CHECK_THAT(p.timeRatio, WithinRel(0.8, 1e-12));
    CHECK_THAT(p.pitchScale, WithinRel(std::exp2(7.0 / 12.0), 1e-12));
    CHECK_THAT(planFor(StretchMode::MasterTempo, 120.0, 120.0, 2400).pitchScale, WithinRel(2.0, 1e-12));
    CHECK_THAT(planFor(StretchMode::MasterTempo, 120.0, 120.0, -2400).pitchScale, WithinRel(0.5, 1e-12));
    CHECK(planFor(StretchMode::MasterTempo, 120.0, 0.0, 0).timeRatio == 1.0);
}

TEST_CASE("stretch mode: sample BPM from the sidecar, bars, or the estimate", "[stretch]")
{
    // 4 s at 120 BPM = 2 bars.
    auto t = sampleTempo(120.0, 0.0, 4.0, 100.0);
    CHECK(t.bpm == 120.0);
    CHECK_THAT(t.bars, WithinRel(2.0, 1e-12));
    CHECK_FALSE(t.estimated);
    t = sampleTempo(0.0, 2.0, 4.0, 100.0); // bars: bpm = 240·bars/length
    CHECK_THAT(t.bpm, WithinRel(120.0, 1e-12));
    CHECK_FALSE(t.estimated);
    // Estimate: 4 s at a 110 BPM project = 1.83 bars → 2 bars → 120 BPM.
    t = sampleTempo(0.0, 0.0, 4.0, 110.0);
    CHECK(t.estimated);
    CHECK(t.bars == 2.0);
    CHECK_THAT(t.bpm, WithinRel(120.0, 1e-12));
    // 3 s at 120 BPM = 1.5 bars → nearest power of two in log terms: 2^round(log2 1.5) = 2^1 = 2.
    t = sampleTempo(0.0, 0.0, 3.0, 120.0);
    CHECK(t.bars == 2.0);
    // Very short / very long: 1/4 … 64 bars.
    CHECK(sampleTempo(0.0, 0.0, 0.05, 120.0).bars == 0.25);
    CHECK(sampleTempo(0.0, 0.0, 600.0, 120.0).bars == 64.0);
    // No length: nothing can be known.
    CHECK(sampleTempo(0.0, 2.0, 0.0, 120.0).bpm == 0.0);
}
