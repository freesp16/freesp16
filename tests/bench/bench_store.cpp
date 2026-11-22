// Store benchmark (saving a full 16-scene project ≤ 150 ms on the host; the save snapshot copy
// ≤ 5 ms). A "full" project uses every scene, track and pattern (randomised settings, 25 % of the step
// cells filled, 15 % of those with p-locks); the worst case fills every cell. Median of N runs; Release only
// (host-release preset). Saves go through the real AtomicWriter (fsync + renames) into the build's artifact folder.
// Loads are reported, not gated (no budget for them; the A9 figure is open until measured on a unit).
#include "tests/unit/store/StoreTestSupport.h"

#include "services/store/CodecRegistry.h"
#include "tests/bench/harness/Bench.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

using namespace services::store;

namespace
{
template <typename Fn>
double medianMs(int runs, Fn&& fn)
{
    std::vector<double> t;
    for (int i = 0; i < runs; ++i)
    {
        const auto a = std::chrono::steady_clock::now();
        fn();
        t.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count());
    }
    std::sort(t.begin(), t.end());
    return t[t.size() / 2];
}

struct Measured
{
    double save, encode, open;
    std::size_t bytes;
};

Measured measure(const char* label, const core::model::Project& p, const juce::File& bundle)
{
    SaveReport report;
    REQUIRE(saveProject(p, bundle, {}, &report).ok()); // warm-up; also creates the old bundle the timed saves replace
    Measured m {};
    m.bytes = report.bytesWritten;
    m.save = medianMs(9, [&] { REQUIRE(saveProject(p, bundle).ok()); });
    std::size_t sink = 0;
    m.encode = medianMs(9, [&] { sink += NativeCodec::encode(p).size(); });
    auto back = std::unique_ptr<core::model::Project>(new core::model::Project);
    m.open = medianMs(5, [&] {
        ImportReport r;
        REQUIRE(openProjectInto(bundle, *back, r).ok());
    });
    CHECK(*back == p);
    std::printf("BENCH store %s: save %.1f ms (encode %.1f ms), open %.1f ms, %zu bytes in 18 files\n", label, m.save,
                m.encode, m.open, m.bytes);
    return m;
}
} // namespace

TEST_CASE("store: save and open a full 16-scene project", "[bench][store]")
{
    storetest::ScratchFolder dir("bench");
    const auto full = storetest::randomProject(2026, 0.25, true);
    std::vector<std::string> problems;
    REQUIRE(core::model::validate(*full, problems));

    auto copy = std::unique_ptr<core::model::Project>(new core::model::Project);
    const double snapshot = medianMs(31, [&] { *copy = *full; });
    std::printf("BENCH store snapshot copy of a full project: %.2f ms (sizeof(Project) = %zu)\n", snapshot,
                sizeof(core::model::Project));
    bench::record("store_snapshot_copy", bench::msToCycles(5.0), bench::msToCycles(snapshot));
    CHECK(snapshot <= 5.0); // 06 §1: the message thread copies, the worker saves

    const auto m = measure("full (25 % steps)", *full, dir / "Full.sixteen");
    bench::record("store_save_full", bench::msToCycles(150.0), bench::msToCycles(m.save));
    bench::record("store_open_full", bench::msToCycles(1000.0), bench::msToCycles(m.open)); // our figure, no budget
    CHECK(m.save <= 150.0); // save budget

    const auto def = core::model::newDefaultProject();
    measure("default", *def, dir / "Default.sixteen");

    const auto worst = storetest::randomProject(2027, 1.0, true); // every cell of every pattern
    measure("worst case (every step)", *worst, dir / "Worst.sixteen");
}
