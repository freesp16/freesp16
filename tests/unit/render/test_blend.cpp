// The soft rasteriser's blend kernels (ui/render/simd/Blend.h): the NEON versions are bit-exact with the scalar
// references (every NEON kernel has a scalar reference, tested against it on the host — Apple silicon runs
// the ARMv7 intrinsics used). Exhaustive over coverage x alpha, random destinations, every length 0..37.
#include "ui/render/simd/Blend.h"

#include <catch2/catch_test_macros.hpp>

#include <random>
#include <vector>

TEST_CASE("render blend: the /255 rounding used by the NEON mask blit is exact for every coverage and alpha",
          "[render][simd]")
{
    for (std::uint32_t c = 0; c < 256; ++c)
        for (std::uint32_t a = 0; a < 256; ++a)
        {
            const std::uint32_t t = c * a + 127;
            REQUIRE(((t + 1 + (t >> 8)) >> 8) == t / 255);
        }
}

#ifdef APP_RENDER_NEON
TEST_CASE("render blend: NEON kernels match the scalar references bit for bit", "[render][simd]")
{
    std::mt19937 rng(7);
    std::vector<std::uint32_t> base(64), a(64), b(64);
    std::vector<std::uint8_t> cov(64);
    for (int n = 0; n <= 37; ++n)
        for (int trial = 0; trial < 200; ++trial)
        {
            for (auto& p : base)
                p = rng() & 0x00FFFFFFu;
            if (trial % 3 == 0)
                for (auto& p : base)
                    p |= 0xFF000000u; // a garbage top byte must not matter where pixels are written
            const std::uint32_t rgb = rng() & 0x00FFFFFFu, a256 = 1 + rng() % 255, alpha = rng() % 256;
            for (auto& c : cov)
                c = static_cast<std::uint8_t>(trial % 4 == 0 ? (rng() % 3 == 0 ? rng() : 0) : rng());
            a = base;
            b = base;
            ui::simd::blendSpanScalar(a.data() + 1, n, rgb, a256);
            ui::simd::blendSpanNeon(b.data() + 1, n, rgb, a256);
            REQUIRE(a == b);
            a = base;
            b = base;
            ui::simd::maskBlitScalar(a.data() + 3, cov.data() + 5, n, rgb, alpha);
            ui::simd::maskBlitNeon(b.data() + 3, cov.data() + 5, n, rgb, alpha);
            REQUIRE(a == b);
        }
    // Exhaustive: every coverage at every alpha, 256 pixels per row.
    std::vector<std::uint8_t> all(256);
    for (int i = 0; i < 256; ++i)
        all[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
    std::vector<std::uint32_t> x(256), y(256);
    for (std::uint32_t alpha = 0; alpha < 256; ++alpha)
    {
        for (int i = 0; i < 256; ++i)
            x[static_cast<std::size_t>(i)] = y[static_cast<std::size_t>(i)] = (rng() & 0x00FFFFFFu);
        ui::simd::maskBlitScalar(x.data(), all.data(), 256, 0x123456u, alpha);
        ui::simd::maskBlitNeon(y.data(), all.data(), 256, 0x123456u, alpha);
        REQUIRE(x == y);
    }
}
#endif

TEST_CASE("render blend: kernels hit the end points exactly", "[render][simd]")
{
    std::uint32_t px[5] = {0x112233u, 0x112233u, 0x112233u, 0x112233u, 0x112233u};
    const std::uint8_t cov[5] = {0, 255, 255, 128, 1};
    ui::simd::maskBlit(px, cov, 5, 0xAABBCCu, 255);
    CHECK(px[0] == 0x112233u); // no coverage: untouched
    CHECK(px[1] == 0xAABBCCu); // full coverage, opaque: the colour
    CHECK(px[2] == 0xAABBCCu);
    CHECK(px[3] != 0x112233u);
    std::uint32_t span[3] = {0, 0, 0};
    ui::simd::blendSpan(span, 3, 0xFFFFFFu, 128);
    CHECK(span[0] == 0x7F7F7Fu);
}
