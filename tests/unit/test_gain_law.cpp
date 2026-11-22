// Gain law of the 0-127 level controls (01-architecture/audio-engine.md "Gain law").
#include "core/util/GainLaw.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using core::GainLaw;

TEST_CASE("gain law anchors", "[gainlaw]")
{
    CHECK(GainLaw::gainExact(0) == 0.0);
    CHECK(GainLaw::gainExact(100) == Catch::Approx(1.0));         // index 100 = 0 dB (default)
    CHECK(GainLaw::gainExact(127) == Catch::Approx(2.0).epsilon(1e-9)); // 127 = x2 (+6.02 dB)
    CHECK(GainLaw::decibels(1) == Catch::Approx(-54.0));
    CHECK(GainLaw::decibels(9) == Catch::Approx(-41.0));
    CHECK(GainLaw::gainExact(50) == Catch::Approx(0.25)); // (i/100)^2
}

TEST_CASE("gain law is monotonic and the table matches the formula", "[gainlaw]")
{
    for (int i = 1; i <= 127; ++i)
    {
        CHECK(GainLaw::gainExact(i) > GainLaw::gainExact(i - 1));
        CHECK(GainLaw::gain(i) == Catch::Approx(GainLaw::gainExact(i)).epsilon(1e-6));
    }
    CHECK(GainLaw::gain(-5) == 0.0f);
    CHECK(GainLaw::gain(500) == GainLaw::gain(127));
}
