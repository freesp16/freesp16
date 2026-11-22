// Amp-envelope time map, storage and sync lengths (ENV-001/002/003/011).
#include "core/util/ampenv/EnvTime.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>

using namespace core::ampenv;

TEST_CASE("ampenv.time: index 0 / 64 / 127 give 0 s, 132.1 ms and the stage maxima", "[ampenv]")
{
    CHECK(envSeconds(EnvStage::Attack, 0, false, 120) == 0.0f);
    CHECK(envSeconds(EnvStage::Attack, 64, false, 120) == Catch::Approx(0.1321).margin(0.00005));
    CHECK(envSeconds(EnvStage::Attack, 1, false, 120) == Catch::Approx(0.0005 * std::pow(64000.0, 1.0 / 127)));
    CHECK(envSeconds(EnvStage::Attack, 127, false, 120) == Catch::Approx(32.0).epsilon(1e-6));
    CHECK(envSeconds(EnvStage::Hold, 0, false, 120) == 0.0f);
    CHECK(envSeconds(EnvStage::Hold, 127, false, 120) == Catch::Approx(10.0).epsilon(1e-6));
    CHECK(envSeconds(EnvStage::Release, 0, false, 120) == 0.0f);
    CHECK(envSeconds(EnvStage::Release, 127, false, 120) == Catch::Approx(20.0).epsilon(1e-6));
    // exponential map: equal index steps are equal ratios
    for (auto s : {EnvStage::Attack, EnvStage::Hold, EnvStage::Release})
        for (int i = 2; i <= 127; ++i)
        {
            const double r = envSecondsExact(s, static_cast<std::uint8_t>(i), false, 120) /
                             envSecondsExact(s, static_cast<std::uint8_t>(i - 1), false, 120);
            CHECK(r == Catch::Approx(envSecondsExact(s, 2, false, 120) / envSecondsExact(s, 1, false, 120)));
        }
}

TEST_CASE("ampenv.time: HOLD and RELEASE have INFINITY at 128, ATTACK clamps to 127", "[ampenv]")
{
    CHECK(std::isinf(envSeconds(EnvStage::Hold, kInfinityIndex, false, 120)));
    CHECK(std::isinf(envSeconds(EnvStage::Release, kInfinityIndex, true, 120)));
    CHECK(envSeconds(EnvStage::Attack, kInfinityIndex, false, 120) == envSeconds(EnvStage::Attack, 127, false, 120));
    CHECK(clampIndex(EnvStage::Attack, 200.0f) == 127);
    CHECK(clampIndex(EnvStage::Hold, 200.0f) == 128);
    CHECK(clampIndex(EnvStage::Release, 127.6f) == 128);
    CHECK(clampIndex(EnvStage::Release, -3.0f) == 0);
    CHECK(clampIndex(EnvStage::Hold, std::nanf("")) == 0);
    CHECK(clampIndex(EnvStage::Attack, 63.5f) == 64);
}

TEST_CASE("ampenv.storage: finite values below 1.0, INFINITY is exactly 1.0 (ENV-001/002)", "[ampenv]")
{
    CHECK(storedFromIndex(0) == 0.0f);
    CHECK(storedFromIndex(64) == 0.5f);
    CHECK(storedFromIndex(127) < 1.0f);
    CHECK(storedFromIndex(kInfinityIndex) == 1.0f);
    for (int i = 0; i <= 128; ++i)
        CHECK(indexFromStored(storedFromIndex(static_cast<std::uint8_t>(i))) == i);
    CHECK(indexFromStored(1.5f) == 128);
    CHECK(indexFromStored(0.9999f) == 127); // a finite stored value never reads back as INFINITY
    CHECK(indexFromStored(-0.1f) == 0);
}

TEST_CASE("ampenv.sync: 16 musical lengths follow the tempo (ENV-003)", "[ampenv]")
{
    CHECK(syncStep(0) == 0);
    CHECK(syncStep(127) == 15);
    CHECK(syncStep(72) == 9);
    CHECK(std::string(syncLabel(9)) == "1/4");
    CHECK(std::string(syncLabel(15)) == "4 BAR");
    CHECK(envSeconds(EnvStage::Attack, 72, true, 120.0) == Catch::Approx(0.5));   // 1/4 at 120 bpm
    CHECK(envSeconds(EnvStage::Release, 127, true, 60.0) == Catch::Approx(16.0)); // 4 bars at 60 bpm
    CHECK(envSeconds(EnvStage::Hold, 0, true, 120.0) == Catch::Approx(0.03125));  // 1/64
    for (int s = 1; s < kSyncSteps; ++s)
        CHECK(syncBeats(s) > syncBeats(s - 1));
    CHECK(envSeconds(EnvStage::Attack, 72, true, 5.0) == envSeconds(EnvStage::Attack, 72, true, 20.0)); // clamped
}

TEST_CASE("ampenv.format: captions of the stage values", "[ampenv]")
{
    char buf[16];
    auto f = [&](EnvStage s, int i, bool sync = false)
    {
        const int n = formatEnvTime(s, static_cast<std::uint8_t>(i), sync, buf, sizeof buf);
        return std::string(buf, static_cast<std::size_t>(n));
    };
    CHECK(f(EnvStage::Attack, 0) == "0 ms");
    CHECK(f(EnvStage::Attack, 1) == "0.5 ms");
    CHECK(f(EnvStage::Attack, 64) == "132 ms");
    CHECK(f(EnvStage::Attack, 127) == "32.0 s");
    CHECK(f(EnvStage::Hold, 128) == "INF");
    CHECK(f(EnvStage::Release, 127) == "20.0 s");
    CHECK(f(EnvStage::Release, 72, true) == "1/4");
}
