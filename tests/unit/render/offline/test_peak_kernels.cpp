// The peak overview's min/max kernel (services/analysis/PeakKernels.h, REN-020), NEON against the scalar
// reference, bit for bit. Header-only, so tests/neon (qemu, armel) compiles this file without the services
// library. On a host without NEON the parity case SKIPs; the scalar case always runs.
#include "core/util/Realtime.h"
#include "services/analysis/PeakKernels.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace k = services::analysis::kernels;

namespace
{
[[maybe_unused]] std::vector<float> noise(int n, std::uint32_t seed)
{
    std::vector<float> v(static_cast<std::size_t>(n));
    std::uint32_t x = seed * 2654435761u + 1;
    for (auto& s : v)
    {
        x ^= x << 13, x ^= x >> 17, x ^= x << 5;
        s = static_cast<float>(static_cast<double>(x) / 4294967295.0 * 2 - 1);
    }
    return v;
}
} // namespace

TEST_CASE("offline render: the scalar min/max kernel finds the extremes", "[analysis][kernels]")
{
    const std::vector<float> v {0.25f, -0.5f, 0.75f, -0.0f, 0.0f, -1.0f, 0.5f};
    float mn = std::numeric_limits<float>::infinity(), mx = -mn;
    k::minMaxScalar(v.data(), static_cast<int>(v.size()), mn, mx);
    CHECK(mn == -1.0f);
    CHECK(mx == 0.75f);
    float zmn = std::numeric_limits<float>::infinity(), zmx = -zmn;
    const float zeros[3] = {-0.0f, 0.0f, -0.0f};
    k::minMaxScalar(zeros, 3, zmn, zmx);
    CHECK_FALSE(std::signbit(zmn)); // −0 maps to +0 (lane-order independent)
    CHECK_FALSE(std::signbit(zmx));
}

TEST_CASE("offline render: the NEON min/max kernel matches the scalar reference bit for bit", "[analysis][kernels]")
{
#if APP_ANALYSIS_HAVE_NEON
    core::rt::enableFlushToZero();
    for (const int n : {0, 1, 7, 8, 9, 31, 256, 257, 1000})
        for (std::uint32_t seed = 1; seed < 6; ++seed)
        {
            auto v = noise(n, seed);
            if (n > 4)
                v[static_cast<std::size_t>(n / 2)] = -0.0f;
            float a0 = std::numeric_limits<float>::infinity(), a1 = -a0, b0 = a0, b1 = a1;
            k::minMaxScalar(v.data(), n, a0, a1);
            k::minMaxNeon(v.data(), n, b0, b1);
            CHECK(std::memcmp(&a0, &b0, 4) == 0);
            CHECK(std::memcmp(&a1, &b1, 4) == 0);
        }
#else
    SKIP("no NEON on this host");
#endif
}
