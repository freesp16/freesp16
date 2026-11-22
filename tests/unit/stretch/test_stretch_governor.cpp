// The MASTER TEMPO governor (engine/voice/stretch/StretchGovernor.h; the cycle budget, FX-864, FX-051 worst case).
#include "engine/voice/stretch/StretchGovernor.h"

#include <catch2/catch_test_macros.hpp>

using namespace engine::voice::stretch;

TEST_CASE("stretch governor: FX-864 score and the low-quality tier", "[stretch][governor]")
{
    CHECK(govern({0, 0, 0}).score == 0);
    CHECK(govern({1, 5, 4}).score == 8 + 5 + 8);
    CHECK(govern({1, 24, 0}).tier == 0); // 32
    CHECK(govern({1, 25, 0}).tier == 1); // 33
    CHECK(govern({0, 1, 16}).tier == 1); // 33: sixteen stretched players alone are "heavy"
    CHECK(govern({0, 0, 16}).tier == 0);
}

TEST_CASE("stretch governor: 8 MASTER TEMPO voices with a typical FX load fit", "[stretch][governor]")
{
    // Typical: Reverb on the send, 6 inserts, 8 stretched voices.
    const auto d = govern({1, 6, 8});
    CHECK_FALSE(d.holdNew);
    CHECK(d.predictedCycles < GovernorConfig {}.deadlineCycles);
}

TEST_CASE("stretch governor: FX-051 worst case holds new MASTER TEMPO voices under the deadline", "[stretch][governor]")
{
    // 32 inserts on Distortion, Reverb, Send Delay (counted as the send — the Reverb is the send here, so the delay
    // runs as an insert-class load) and a Master Compressor: 8 + 32 + 1 = 41 → tier 1.
    const GovernorConfig cfg;
    GovernorInput in {1, 33, 0};
    int admitted = 0;
    // Sixteen notes start one after the other; each asks the governor whether one more fits.
    for (int v = 0; v < 16; ++v)
    {
        in.masterTempoVoices = admitted;
        const auto d = govern(in, cfg);
        CHECK(d.tier == 1);
        if (! d.holdNew)
            ++admitted;
    }
    in.masterTempoVoices = admitted;
    const auto d = govern(in, cfg);
    INFO("admitted " << admitted << " predicted " << d.predictedCycles);
    CHECK(admitted < 16);
    CHECK(d.predictedCycles <= cfg.deadlineCycles);
    // Without the FX the governor admits all 16 and switches to the cheaper tier (score 32 + … ≥ 33 at 16 voices +).
    GovernorInput light {0, 2, 0};
    int all = 0;
    for (int v = 0; v < 16; ++v)
    {
        light.masterTempoVoices = all;
        if (! govern(light, cfg).holdNew)
            ++all;
    }
    CHECK(all == 16);
}
