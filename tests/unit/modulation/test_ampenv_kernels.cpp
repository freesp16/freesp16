// Amp-envelope gain kernels: NEON against the scalar references, every length 0…67, aligned and unaligned.
#include "engine/modulation/AmpEnvelopeKernels.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <random>
#include <vector>

namespace envk = engine::modulation::envk;

namespace
{
std::vector<float> noise(int n, unsigned seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> d(-1.0f, 1.0f);
    std::vector<float> v(static_cast<std::size_t>(n) + 4);
    for (auto& x : v)
        x = d(rng);
    return v;
}
bool close(float a, float b)
{
    return std::fabs(a - b) <= 1e-6f * std::max(1.0f, std::fabs(b));
}

void compare(const envk::KernelSet& x, const envk::KernelSet& y)
{
    const envk::GeoSeed seed {{-0.9f * 0.999f, -0.9f * 0.999f * 0.999f, -0.9f * 0.997003f, -0.9f * 0.996006f},
                             0.996006f};
    for (int n = 0; n <= 67; ++n)
        for (int off = 0; off < 2; ++off)
        {
            INFO("n " << n << " offset " << off);
            std::vector<float> g1(80, 7.0f), g2(80, 7.0f);
            x.fillConstant(g1.data() + off, n, 0.3f);
            y.fillConstant(g2.data() + off, n, 0.3f);
            CHECK(g1 == g2);
            x.fillLinear(g1.data() + off, n, 0.25f, -0.0031f, 17.0f);
            y.fillLinear(g2.data() + off, n, 0.25f, -0.0031f, 17.0f);
            for (int i = 0; i < 80; ++i)
                CHECK(close(g1[static_cast<std::size_t>(i)], g2[static_cast<std::size_t>(i)]));
            x.fillGeometric(g1.data() + off, n, 1.0f, seed);
            y.fillGeometric(g2.data() + off, n, 1.0f, seed);
            for (int i = 0; i < 80; ++i)
                CHECK(close(g1[static_cast<std::size_t>(i)], g2[static_cast<std::size_t>(i)]));
            auto l1 = noise(n, 1u + static_cast<unsigned>(n)), r1 = noise(n, 99u + static_cast<unsigned>(n));
            auto l2 = l1, r2 = r1;
            x.applyGain(l1.data() + off, r1.data() + off, g1.data(), n);
            y.applyGain(l2.data() + off, r2.data() + off, g1.data(), n);
            CHECK(l1 == l2);
            CHECK(r1 == r2);
            x.applyConstant(l1.data() + off, r1.data() + off, 0.7f, n);
            y.applyConstant(l2.data() + off, r2.data() + off, 0.7f, n);
            CHECK(l1 == l2);
            CHECK(r1 == r2);
        }
}
} // namespace

TEST_CASE("ampenv.kernels: scalar references compute the documented formulas", "[ampenv]")
{
    const auto& k = envk::scalarKernels();
    float g[9];
    k.fillLinear(g, 9, 1.0f, 0.5f, 1.0f);
    CHECK(g[0] == 1.5f);
    CHECK(g[8] == 5.5f);
    const double rho = 0.9;
    const envk::GeoSeed s {{0.9f, 0.81f, 0.729f, 0.6561f}, static_cast<float>(rho * rho * rho * rho)};
    k.fillGeometric(g, 9, 0.0f, s);
    for (int i = 0; i < 9; ++i)
        CHECK(std::fabs(static_cast<double>(g[i]) - std::pow(rho, i + 1)) < 1e-6);
    float l[3] = {1, 2, 3}, r[3] = {-1, -2, -3};
    k.applyGain(l, r, g, 3);
    CHECK(l[1] == 2.0f * g[1]);
    CHECK(r[2] == -3.0f * g[2]);
}

TEST_CASE("ampenv.kernels: NEON kernels match the scalar references within 1e-6", "[ampenv]")
{
    if (! engine::simd::hasNeon())
        SKIP("no NEON on this host");
    compare(envk::activeKernels(), envk::scalarKernels());
    CHECK(&envk::activeKernels() != &envk::scalarKernels());
}
