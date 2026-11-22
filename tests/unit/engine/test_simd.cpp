// NEON kernels against their scalar references: same results on random data for every
// length 0…67 (body + tail), aligned and unaligned. On a host without NEON only the scalar references are checked.
#include "engine/dsp/simd/Kernels.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <random>
#include <vector>

namespace
{
[[maybe_unused]] std::vector<float> noise(int n, unsigned seed, float scale = 1.0f)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> d(-scale, scale);
    std::vector<float> v(static_cast<std::size_t>(n) + 1); // + 1: room for the unaligned offset
    for (auto& x : v)
        x = d(rng);
    return v;
}
bool close(float a, float b, float rel = 1e-6f) { return std::fabs(a - b) <= rel * std::max(1.0f, std::fabs(b)); }
} // namespace

TEST_CASE("simd: scalar references", "[engine][simd]")
{
    const float x[5] = {0.5f, -0.75f, 0.25f, 0.0f, -0.1f};
    const auto ps = engine::simd::scalar::peakSum(x, 5);
    CHECK(ps.peak == 0.75f);
    CHECK(close(ps.sumSquares, 0.25f + 0.5625f + 0.0625f + 0.01f));
    CHECK(engine::simd::scalar::peakAbs(x, 5) == 0.75f);
    CHECK(engine::simd::scalar::peakAbs(x, 0) == 0.0f);
    float d[5] = {1, 1, 1, 1, 1};
    engine::simd::scalar::add(d, x, 5);
    CHECK(d[1] == 0.25f);
    engine::simd::scalar::scale(d, x, 2.0f, 5);
    CHECK(d[1] == -1.5f);
}

#if APP_ENGINE_HAVE_NEON
TEST_CASE("simd: NEON kernels match the scalar references", "[engine][simd]")
{
    namespace s = engine::simd::scalar;
    namespace v = engine::simd::neon;
    for (int n = 0; n <= 67; ++n)
        for (int offset = 0; offset <= 1; ++offset) // offset 1: unaligned loads/stores
        {
            INFO("n = " << n << ", offset = " << offset);
            const auto x = noise(n, 11u + static_cast<unsigned>(n));
            const auto g = noise(n, 99u + static_cast<unsigned>(n), 2.0f);
            const float* src = x.data() + offset;
            const float* gain = g.data() + offset;

            const auto a = s::peakSum(src, n), b = v::peakSum(src, n);
            CHECK(a.peak == b.peak); // max/abs are exact
            CHECK(close(a.sumSquares, b.sumSquares, 1e-5f)); // summation order differs
            CHECK(s::peakAbs(src, n) == v::peakAbs(src, n));

            std::vector<float> d1(x.size(), 0.5f), d2(x.size(), 0.5f);
            s::add(d1.data() + offset, src, n);
            v::add(d2.data() + offset, src, n);
            CHECK(d1 == d2);
            s::scale(d1.data() + offset, src, 0.3f, n);
            v::scale(d2.data() + offset, src, 0.3f, n);
            CHECK(d1 == d2);
            s::multiply(d1.data() + offset, gain, n);
            v::multiply(d2.data() + offset, gain, n);
            CHECK(d1 == d2);
            s::mulAdd(d1.data() + offset, src, gain, n);
            v::mulAdd(d2.data() + offset, src, gain, n);
            for (std::size_t i = 0; i < d1.size(); ++i)
                CHECK(close(d1[i], d2[i], 1e-6f)); // a contracted scalar FMA may differ by 1 ulp
        }
}
#endif

TEST_CASE("simd: the dispatcher uses NEON where available", "[engine][simd]")
{
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    STATIC_REQUIRE(engine::simd::hasNeon());
#else
    STATIC_REQUIRE_FALSE(engine::simd::hasNeon());
#endif
    alignas(16) float block[64] {};
    block[37] = -0.9f;
    CHECK(engine::simd::peakAbs(block, 64) == 0.9f);
}
