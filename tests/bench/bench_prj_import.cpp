// bench_prj_import: tree + mapping of a synthetic full-size reference project
// (tests/unit/interop/synthetic/large.json: 64 patterns, ~60 % of the cells triggered with up to 8 locks each, ≈ 800
// KB on disk — larger than the 756 KB project of the research), sample copying excluded. Target ≤ 2 s on the device;
// desktop numbers are relative only. Also the FMT-043 round-trip check the import runs.
#include "core/model/ModelOps.h"
#include "services/interop/sp16/Sp16Import.h"
#include "services/interop/sp16/Sp16ProjectCodec.h"
#include "tests/unit/interop/Fixtures.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdio>

using namespace services::interop::sp16;

TEST_CASE("bench prj_import: decode + map a full-size synthetic project", "[bench]")
{
    INTEROP_FIXTURE(g, "large");
    const auto bytes = interop_test::bytesOf(g.prj);
    auto project = core::model::newDefaultProject();
    std::vector<double> inflate, decode, map, roundTrip;
    std::size_t inflated = 0;
    MapStats stats;
    for (int run = 0; run < 7; ++run)
    {
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        auto raw = inflateZlib(std::span(bytes).subspan(8), 64u << 20);
        REQUIRE(raw);
        inflated = raw.value().size();
        const double t1 = juce::Time::getMillisecondCounterHiRes();
        auto tree = decodeTree(raw.value());
        REQUIRE(tree);
        const double t2 = juce::Time::getMillisecondCounterHiRes();
        ImportLog log;
        MapOptions mo;
        REQUIRE(mapProject(tree.value(), *project, mo, log, &stats));
        const double t3 = juce::Time::getMillisecondCounterHiRes();
        const bool same = writeContainer(tree.value()) == bytes;
        const double t4 = juce::Time::getMillisecondCounterHiRes();
        CHECK(same);
        inflate.push_back(t1 - t0);
        decode.push_back(t2 - t1);
        map.push_back(t3 - t2);
        roundTrip.push_back(t4 - t3);
    }
    auto median = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    std::printf("bench_prj_import: file %zu B, inflated %zu B, patterns %d, triggers %d, lock cells %d, locks %d\n"
                "  inflate %.1f ms, tree %.1f ms, map %.1f ms (total %.1f ms; target <= 2000 ms on the A9), "
                "round-trip check %.1f ms (desktop, median of 7)\n",
                bytes.size(), inflated, stats.patterns, stats.triggers, stats.lockCells, stats.locks, median(inflate),
                median(decode), median(map), median(inflate) + median(decode) + median(map), median(roundTrip));
    CHECK(stats.patterns == 64);
    CHECK(stats.verbatimPatterns == 0);
    CHECK(median(inflate) + median(decode) + median(map) < 1000.0); // desktop sanity bound, not the device budget
}
