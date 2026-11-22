// The software rasteriser's two hot loops (NEON where it pays, each kernel with a scalar reference; the
// NEON versions are bit-exact with the references — tests/unit/render/test_blend.cpp, tests/bench/bench_render.cpp).
// Pixels are 0x00RRGGBB; every written pixel has a zero top byte.
//   blendSpan: n pixels toward the constant colour `rgb` with weight a256 in 1..255 (256 - a256 for the destination)
//   maskBlit:  n pixels toward `rgb` with the per-pixel weight of an 8-bit coverage mask times `alpha` (0..255):
//              a = round(cov * alpha / 255) as (cov * alpha + 127) / 255, then a += a >> 7 (255 → 256); cov 0 leaves
//              the destination untouched
#pragma once

#include <cstdint>

namespace ui::simd
{
void blendSpanScalar(std::uint32_t* dst, int n, std::uint32_t rgb, std::uint32_t a256) noexcept;
void maskBlitScalar(std::uint32_t* dst, const std::uint8_t* cov, int n, std::uint32_t rgb,
                    std::uint32_t alpha) noexcept;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#define APP_RENDER_NEON 1
void blendSpanNeon(std::uint32_t* dst, int n, std::uint32_t rgb, std::uint32_t a256) noexcept;
void maskBlitNeon(std::uint32_t* dst, const std::uint8_t* cov, int n, std::uint32_t rgb, std::uint32_t alpha) noexcept;
#endif

inline void blendSpan(std::uint32_t* dst, int n, std::uint32_t rgb, std::uint32_t a256) noexcept
{
#ifdef APP_RENDER_NEON
    blendSpanNeon(dst, n, rgb, a256);
#else
    blendSpanScalar(dst, n, rgb, a256);
#endif
}

inline void maskBlit(std::uint32_t* dst, const std::uint8_t* cov, int n, std::uint32_t rgb,
                     std::uint32_t alpha) noexcept
{
#ifdef APP_RENDER_NEON
    maskBlitNeon(dst, cov, n, rgb, alpha);
#else
    maskBlitScalar(dst, cov, n, rgb, alpha);
#endif
}

// One pixel toward src with weight a256 (0..256); also the tail of the kernels.
inline std::uint32_t blendPixel(std::uint32_t dst, std::uint32_t src, std::uint32_t a256) noexcept
{
    const std::uint32_t inv = 256 - a256;
    const std::uint32_t rb = (((src & 0xFF00FFu) * a256 + (dst & 0xFF00FFu) * inv) >> 8) & 0xFF00FFu;
    const std::uint32_t g = (((src & 0x00FF00u) * a256 + (dst & 0x00FF00u) * inv) >> 8) & 0x00FF00u;
    return rb | g;
}
} // namespace ui::simd
