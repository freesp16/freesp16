// Stretch kernels (engine/voice/stretch/StretchKernels.h, StretchFft.h): the scalar references against plain maths,
// the FFT against a direct DFT, and the NEON kernels against the scalar references on random data for every length
// 0…67, aligned and unaligned. On a host without NEON the NEON case SKIPs (x86-64); scripts/neon-tests.sh runs this
// file under qemu-arm as a Cortex-A9.
#include "engine/voice/stretch/StretchFft.h"
#include "engine/voice/stretch/StretchKernels.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <random>
#include <vector>

namespace
{
namespace k = engine::voice::stretch::kernels;

std::vector<float> noise(int n, unsigned seed, float scale = 1.0f)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> d(-scale, scale);
    std::vector<float> v(static_cast<std::size_t>(n) + 1);
    for (auto& x : v)
        x = d(rng);
    return v;
}
bool close(double a, double b, double tol) { return std::fabs(a - b) <= tol * std::max(1.0, std::fabs(b)); }
} // namespace

TEST_CASE("stretch kernels: scalar references", "[stretch][kernels]")
{
    const auto a = noise(67, 1), b = noise(67, 2), w = noise(67, 3);
    for (int n : {0, 1, 3, 4, 5, 64, 67})
    {
        INFO("n = " << n);
        double ref = 0.0;
        for (int i = 0; i < n; ++i)
            ref += static_cast<double>(a[i]) * static_cast<double>(b[i]);
        CHECK(close(static_cast<double>(k::scalar::dot(a.data(), b.data(), n)), ref, 1e-5));
        std::vector<float> d(68, 0.25f);
        k::scalar::mulWindow(d.data(), a.data(), w.data(), n);
        for (int i = 0; i < n; ++i)
            CHECK(d[i] == a[i] * w[i]);
        k::scalar::addWindowed(d.data(), b.data(), w.data(), n);
        for (int i = 0; i < n; ++i)
            CHECK(d[i] == a[i] * w[i] + b[i] * w[i]);
    }
    // atan2 against the library, every quadrant, the axes and (0, 0).
    std::vector<float> ys, xs;
    for (int i = 0; i < 400; ++i)
    {
        const double ang = -k::kPiD + 2.0 * k::kPiD * i / 399.0;
        const double r = 1.0e-3 + i * 0.01;
        ys.push_back(static_cast<float>(r * std::sin(ang)));
        xs.push_back(static_cast<float>(r * std::cos(ang)));
    }
    for (float v : {0.0f, 1.0f, -1.0f})
        for (float u : {0.0f, 2.0f, -2.0f})
        {
            ys.push_back(v);
            xs.push_back(u);
        }
    std::vector<float> out(ys.size());
    k::scalar::atan2(out.data(), ys.data(), xs.data(), static_cast<int>(ys.size()));
    for (std::size_t i = 0; i < ys.size(); ++i)
    {
        INFO("y " << ys[i] << " x " << xs[i]);
        const double ref = (ys[i] == 0.0f && xs[i] == 0.0f) ? 0.0 : std::atan2(static_cast<double>(ys[i]), static_cast<double>(xs[i]));
        double d = std::fabs(static_cast<double>(out[i]) - ref);
        d = std::min(d, std::fabs(d - 2.0 * k::kPiD)); // ±π is the same angle
        CHECK(d < 1.0e-6);
    }
    // sinCos and princarg.
    for (int i = -1000; i <= 1000; ++i)
    {
        const float th = static_cast<float>(i) * (k::kPi / 1000.0f);
        float s = 0, c = 0;
        k::sinCos(th, s, c);
        CHECK(std::fabs(static_cast<double>(s) - std::sin(static_cast<double>(th))) < 4e-7);
        CHECK(std::fabs(static_cast<double>(c) - std::cos(static_cast<double>(th))) < 4e-7);
        const float wrapped = k::princarg(th + 6.0f * k::kTwoPi);
        const float diff = std::fabs(wrapped - th);
        CHECK((diff < 1e-4f || std::fabs(diff - k::kTwoPi) < 1e-4f)); // ±π are the same angle
        CHECK(std::fabs(k::princarg(th * 37.0f)) <= k::kPi + 1e-5f);
    }
}

TEST_CASE("stretch kernels: the FFT matches a direct DFT and inverts", "[stretch][kernels]")
{
    for (int n : {4, 8, 64, 1024})
    {
        INFO("N = " << n);
        engine::voice::stretch::StretchFft fft;
        fft.prepare(n);
        REQUIRE(fft.size() == n);
        const auto xr = noise(n, 7u + static_cast<unsigned>(n)), xi = noise(n, 9u + static_cast<unsigned>(n));
        std::vector<float> re(xr.begin(), xr.begin() + n), im(xi.begin(), xi.begin() + n);
        fft.forward(re.data(), im.data());
        double worst = 0.0;
        for (int kk = 0; kk < n; ++kk)
        {
            double sr = 0.0, si = 0.0;
            for (int t = 0; t < n; ++t)
            {
                const double a = -2.0 * k::kPiD * kk * t / n;
                sr += static_cast<double>(xr[static_cast<std::size_t>(t)]) * std::cos(a) - static_cast<double>(xi[static_cast<std::size_t>(t)]) * std::sin(a);
                si += static_cast<double>(xr[static_cast<std::size_t>(t)]) * std::sin(a) + static_cast<double>(xi[static_cast<std::size_t>(t)]) * std::cos(a);
            }
            worst = std::max({worst, std::fabs(sr - static_cast<double>(re[static_cast<std::size_t>(kk)])),
                              std::fabs(si - static_cast<double>(im[static_cast<std::size_t>(kk)]))});
        }
        CHECK(worst < 1e-4 * n);
        fft.inverse(re.data(), im.data());
        for (int t = 0; t < n; ++t)
        {
            CHECK(std::fabs(re[static_cast<std::size_t>(t)] / static_cast<float>(n) - xr[static_cast<std::size_t>(t)]) < 1e-5f);
            CHECK(std::fabs(im[static_cast<std::size_t>(t)] / static_cast<float>(n) - xi[static_cast<std::size_t>(t)]) < 1e-5f);
        }
    }
}

TEST_CASE("stretch kernels: NEON kernels match the scalar references", "[stretch][kernels]")
{
    if (! engine::simd::hasNeon())
        SKIP("no NEON on this host");
#if APP_ENGINE_HAVE_NEON
    namespace s = k::scalar;
    namespace v = k::neon;
    for (int n = 0; n <= 67; ++n)
        for (int offset = 0; offset <= 1; ++offset)
        {
            INFO("n = " << n << ", offset = " << offset);
            const auto x = noise(n, 11u + static_cast<unsigned>(n)), y = noise(n, 13u + static_cast<unsigned>(n));
            const auto w = noise(n, 17u + static_cast<unsigned>(n)), z = noise(n, 19u + static_cast<unsigned>(n));
            const float* a = x.data() + offset;
            const float* b = y.data() + offset;
            CHECK(close(static_cast<double>(s::dot(a, b, n)), static_cast<double>(v::dot(a, b, n)), 1e-6));
            std::vector<float> d1(x.size(), 0.5f), d2(x.size(), 0.5f);
            s::mulWindow(d1.data() + offset, a, w.data() + offset, n);
            v::mulWindow(d2.data() + offset, a, w.data() + offset, n);
            CHECK(d1 == d2);
            s::addWindowed(d1.data() + offset, b, w.data() + offset, n);
            v::addWindowed(d2.data() + offset, b, w.data() + offset, n);
            CHECK(d1 == d2);
            std::vector<float> m1(x.size()), m2(x.size());
            s::magSq(m1.data(), a, b, n);
            v::magSq(m2.data(), a, b, n);
            for (int i = 0; i < n; ++i)
                CHECK(close(static_cast<double>(m1[i]), static_cast<double>(m2[i]), 1e-6));
            s::atan2(m1.data(), a, b, n);
            v::atan2(m2.data(), a, b, n);
            for (int i = 0; i < n; ++i)
                CHECK(std::fabs(m1[i] - m2[i]) < 1e-6f);
            std::vector<float> r1(x.begin(), x.end()), i1(y.begin(), y.end()), r2 = r1, i2 = i1;
            s::rotate(r1.data() + offset, i1.data() + offset, w.data() + offset, z.data() + offset, n);
            v::rotate(r2.data() + offset, i2.data() + offset, w.data() + offset, z.data() + offset, n);
            for (std::size_t i = 0; i < r1.size(); ++i)
            {
                CHECK(close(static_cast<double>(r1[i]), static_cast<double>(r2[i]), 1e-6));
                CHECK(close(static_cast<double>(i1[i]), static_cast<double>(i2[i]), 1e-6));
            }
        }
    // One FFT stage: half = 4, 8, 32 over 64 points.
    for (int half : {4, 8, 32})
    {
        INFO("half = " << half);
        const auto xr = noise(64, 23), xi = noise(64, 29), wr = noise(half, 31), wi = noise(half, 37);
        std::vector<float> r1(xr.begin(), xr.begin() + 64), i1(xi.begin(), xi.begin() + 64), r2 = r1, i2 = i1;
        s::butterflies(r1.data(), i1.data(), wr.data(), wi.data(), half, 64);
        v::butterflies(r2.data(), i2.data(), wr.data(), wi.data(), half, 64);
        for (int i = 0; i < 64; ++i)
        {
            CHECK(close(static_cast<double>(r1[i]), static_cast<double>(r2[i]), 1e-6));
            CHECK(close(static_cast<double>(i1[i]), static_cast<double>(i2[i]), 1e-6));
        }
    }
#endif
}
