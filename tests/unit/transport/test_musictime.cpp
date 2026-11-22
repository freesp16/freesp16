// core/util/musictime: BAR:BEAT:STEP, ppqPerFrame, the frame rule shared by every engine stage, tap
// tempo (SYN-003).
#include "core/util/musictime/MusicTime.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace mt = core::musictime;

namespace
{
std::string text(const mt::Position& p)
{
    char buf[16];
    mt::formatPosition(p, buf, sizeof(buf));
    return buf;
}
} // namespace

TEST_CASE("transport.musictime: toPosition gives 1-based BAR:BEAT:STEP from the pattern PPQ", "[transport]")
{
    static_assert(mt::toPosition(0.0) == mt::Position {1, 1, 1});
    static_assert(mt::toPosition(0.25) == mt::Position {1, 1, 2});
    static_assert(mt::toPosition(1.0) == mt::Position {1, 2, 1});
    static_assert(mt::toPosition(3.75) == mt::Position {1, 4, 4});
    static_assert(mt::toPosition(4.0) == mt::Position {2, 1, 1});
    static_assert(mt::toPosition(63.99) == mt::Position {16, 4, 4});
    CHECK(mt::toPosition(0.9999999999) == mt::Position {1, 2, 1}); // rounding of a computed PPQ
    CHECK(mt::toPosition(0.2499) == mt::Position {1, 1, 1});
    // Pre-count (negative PPQ): bars left, counted −N … −1.
    CHECK(mt::toPosition(-8.0) == mt::Position {-2, 1, 1});
    CHECK(mt::toPosition(-4.0) == mt::Position {-1, 1, 1});
    CHECK(mt::toPosition(-0.25) == mt::Position {-1, 4, 4});
    CHECK(text(mt::toPosition(0.0)) == "001:01:01");
    CHECK(text(mt::toPosition(16.5)) == "005:01:03");
    CHECK(text(mt::toPosition(-8.0)) == "-02:01:01");
}

TEST_CASE("transport.musictime: ppqPerFrame and the BPM grid", "[transport]")
{
    static_assert(mt::kQuartersPerStep == 0.25);
    CHECK(mt::ppqPerFrame(120.0, 44100.0) == 120.0 / (60.0 * 44100.0));
    CHECK(22050.0 * mt::ppqPerFrame(120.0, 44100.0) == 1.0);
    CHECK(mt::clampBpm(10.0) == 20.0);
    CHECK(mt::clampBpm(301.0) == 300.0);
    CHECK(mt::clampBpm(120.04) == 120.0);
    CHECK(mt::clampBpm(120.06) == 120.1);
    CHECK(mt::clampBpm(std::nan("")) == 120.0);
}

TEST_CASE("transport.musictime: frameOf rounds up to the first frame at or after the event", "[transport]")
{
    const double ppf = mt::ppqPerFrame(120.0, 44100.0);
    CHECK(mt::frameOf(1.0, 1.0 - 34 * ppf, ppf) == 34);        // exactly on a frame
    CHECK(mt::frameOf(1.0, 1.0 - 34.5 * ppf, ppf) == 35);      // between frames: the next one
    CHECK(mt::frameOf(1.0, 1.0 + ppf, ppf) == 0);              // before the block: clamped
    CHECK_FALSE(mt::inBlock(1.0, 1.0 + 1.5 * ppf, ppf, 64));  // … and it belongs to an earlier block
    CHECK(mt::inBlock(1.0, 1.0 + 0.5 * ppf, ppf, 64));        // between this block's frame −1 and 0: frame 0
    CHECK(mt::inBlock(1.0, 1.0 - 63 * ppf, ppf, 64));
    CHECK_FALSE(mt::inBlock(1.0, 1.0 - 64 * ppf, ppf, 64));
}

TEST_CASE("transport.musictime: consecutive blocks see every grid point exactly once", "[transport]")
{
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> bpm(20.0, 300.0), start(-9.0, 1000.0);
    for (int run = 0; run < 100; ++run)
    {
        const double ppf = mt::ppqPerFrame(bpm(rng), 44100.0);
        const double origin = start(rng);
        for (const double unit : {1.0, 0.25, 4.0})
        {
            std::vector<double> seen;
            std::vector<std::int64_t> frames;
            const int blocks = static_cast<int>(3.0 * unit / (64.0 * ppf)) + 64; // ≥ 3 grid points
            for (int b = 0; b < blocks; ++b)
            {
                const double p0 = origin + static_cast<double>(b) * 64.0 * ppf;
                mt::forEachGridPoint(p0, ppf, 64, unit,
                                     [&](double k, int f)
                                     {
                                         seen.push_back(k);
                                         frames.push_back(static_cast<std::int64_t>(b) * 64 + f);
                                     });
            }
            // Every multiple of unit in (first, last) appears once, in order, on its frame ⌈(k − origin) / ppf⌉.
            REQUIRE(seen.size() >= 2);
            for (std::size_t i = 1; i < seen.size(); ++i)
                REQUIRE(seen[i] - seen[i - 1] == unit);
            for (std::size_t i = 0; i < seen.size(); ++i)
            {
                const double exact = (seen[i] - origin) / ppf;
                REQUIRE(static_cast<double>(frames[i]) >= exact - 1e-3);
                REQUIRE(static_cast<double>(frames[i]) < exact + 1.0);
            }
        }
    }
}

TEST_CASE("transport.tap: 500 ms taps give 120.0 BPM from the second tap; 2 s without a tap restarts", "[transport]")
{
    mt::TapTempo tap;
    CHECK_FALSE(tap.tap(1000.0).has_value()); // one tap is not a tempo
    const auto second = tap.tap(1500.0);
    REQUIRE(second.has_value());
    CHECK(*second == 120.0);
    CHECK(tap.tap(2000.0).value() == 120.0);
    // The average of the last 4 intervals: 500, 500, 400, 400 → 450 ms → 133.33 BPM.
    tap.tap(2400.0);
    CHECK(std::fabs(tap.tap(2800.0).value() - 60000.0 / 450.0) < 1e-9);
    // Four intervals of 400 ms: the old ones dropped out.
    tap.tap(3200.0);
    CHECK(tap.tap(3600.0).value() == 150.0);
    // A pause > 2 s starts a new series.
    CHECK_FALSE(tap.tap(5601.0).has_value());
    CHECK(tap.taps() == 1);
    CHECK(tap.tap(6101.0).value() == 120.0);
}
