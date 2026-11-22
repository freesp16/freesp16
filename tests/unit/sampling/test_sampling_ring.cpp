// The capture ring (one ring, two readers): one writer, readers by absolute frame index,
// wrap-around, "already overwritten" and "not written yet" refusals, and a concurrent writer/reader that never returns
// torn data as valid.
#include "core/util/sampling/CaptureHub.h"
#include "engine/sampling/CaptureRing.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <vector>

using core::sampling::CaptureRing;

namespace
{
// Frame value of absolute index i (L) and its negative (R).
float valueOf(std::int64_t i) { return static_cast<float>(i % 1000003) * 1e-6f; }

void writeFrames(CaptureRing& r, std::int64_t from, int n)
{
    std::vector<float> l(static_cast<std::size_t>(n)), rr(static_cast<std::size_t>(n));
    for (int k = 0; k < n; ++k)
    {
        l[static_cast<std::size_t>(k)] = valueOf(from + k);
        rr[static_cast<std::size_t>(k)] = -valueOf(from + k);
    }
    r.write(l.data(), rr.data(), n);
}
} // namespace

TEST_CASE("sampling.ring: frames read back by absolute index across the wrap", "[sampling][ring]")
{
    CaptureRing r;
    REQUIRE(r.allocate(20000));
    CHECK(r.capacity() == 20000);
    CHECK(r.head() == 0);
    std::int64_t h = 0;
    while (h < 50000) // 2.5 × the capacity, 64-frame blocks
    {
        writeFrames(r, h, 64);
        h += 64;
    }
    CHECK(r.head() == h);
    std::vector<float> l(1000), rr(1000);
    const std::int64_t from = h - 1000; // spans the wrap point at 40000 … no: any recent window
    REQUIRE(r.read(from, 1000, l.data(), rr.data()));
    for (int k = 0; k < 1000; ++k)
    {
        REQUIRE(l[static_cast<std::size_t>(k)] == valueOf(from + k));
        REQUIRE(rr[static_cast<std::size_t>(k)] == -valueOf(from + k));
    }
    // A window that crosses the physical end of the buffer (index 40000 = 2 × capacity).
    REQUIRE(r.read(39950, 100, l.data(), nullptr));
    for (int k = 0; k < 100; ++k)
        REQUIRE(l[static_cast<std::size_t>(k)] == valueOf(39950 + k));
}

TEST_CASE("sampling.ring: refuses frames not written yet or overwritten (with the write guard)", "[sampling][ring]")
{
    CaptureRing r;
    REQUIRE(r.allocate(20000));
    for (std::int64_t h = 0; h < 30016; h += 64)
        writeFrames(r, h, 64);
    float buf[64];
    CHECK_FALSE(r.read(r.head() - 32, 64, buf, buf)); // half not written yet
    CHECK(r.read(r.head() - 64, 64, buf, nullptr));
    CHECK(r.oldest() == r.head() + CaptureRing::kMaxWrite - r.capacity());
    CHECK_FALSE(r.read(r.oldest() - 1, 64, buf, nullptr)); // inside the guard / overwritten
    CHECK(r.read(r.oldest(), 64, buf, nullptr));
    CHECK_FALSE(r.read(0, 64, buf, nullptr));
    CHECK(r.read(5, 0, nullptr, nullptr));
    CHECK_FALSE(r.read(-1, 1, buf, nullptr));
}

TEST_CASE("sampling.ring: a mono write fills both sides; the hub's reference count deletes it once", "[sampling][ring]")
{
    auto* hub = new core::sampling::CaptureHub();
    REQUIRE(hub->ring.allocate(10000));
    float x[64];
    for (int i = 0; i < 64; ++i)
        x[i] = static_cast<float>(i);
    hub->ring.write(x, nullptr, 64);
    float l[64], r[64];
    REQUIRE(hub->ring.read(0, 64, l, r));
    CHECK(l[63] == 63.0f);
    CHECK(r[63] == 63.0f);
    hub->retain();
    core::sampling::releaseCaptureHub(hub); // the engine's
    CHECK(hub->refs.load() == 1);
    hub->release(); // the creator's: deleted (ASan / LSan check the rest)
}

TEST_CASE("sampling.ring: a concurrent reader never accepts a torn window", "[sampling][ring]")
{
    CaptureRing r;
    REQUIRE(r.allocate(8192 + CaptureRing::kMaxWrite));
    std::atomic<bool> stop {false};
    std::thread writer([&] {
        std::int64_t h = 0;
        while (! stop.load(std::memory_order_relaxed) && h < 4'000'000)
        {
            writeFrames(r, h, 64);
            h += 64;
        }
        stop = true;
    });
    std::vector<float> l(2048);
    std::uint64_t accepted = 0, refused = 0;
    std::int64_t torn = -1;
    while (! stop.load(std::memory_order_relaxed) && torn < 0)
    {
        const std::int64_t h = r.head();
        const std::int64_t from = h > 6000 ? h - 6000 : 0; // close to the oldest edge: the writer races the copy
        if (r.read(from, 2048, l.data(), nullptr))
        {
            ++accepted;
            for (int k = 0; k < 2048; ++k)
                if (l[static_cast<std::size_t>(k)] != valueOf(from + k))
                {
                    torn = from + k;
                    break;
                }
        }
        else
            ++refused;
    }
    stop = true;
    writer.join();
    CHECK(torn == -1);
    INFO("accepted " << accepted << " refused " << refused);
    CHECK(accepted > 0);
}
