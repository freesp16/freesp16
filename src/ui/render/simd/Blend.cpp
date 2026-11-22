#include "ui/render/simd/Blend.h"

#ifdef APP_RENDER_NEON
#include <arm_neon.h>
#endif

namespace ui::simd
{
void blendSpanScalar(std::uint32_t* dst, int n, std::uint32_t rgb, std::uint32_t a256) noexcept
{
    for (int i = 0; i < n; ++i)
        dst[i] = blendPixel(dst[i], rgb, a256);
}

void maskBlitScalar(std::uint32_t* dst, const std::uint8_t* cov, int n, std::uint32_t rgb, std::uint32_t alpha) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const std::uint32_t c = cov[i];
        if (c == 0)
            continue;
        std::uint32_t a = (c * alpha + 127) / 255;
        a += a >> 7; // 255 -> 256
        dst[i] = a >= 256 ? rgb : blendPixel(dst[i], rgb, a);
    }
}

#ifdef APP_RENDER_NEON
// Per channel in 16-bit lanes: (s * a + d * (256 - a)) >> 8 never exceeds 255 * 256, the same value the scalar
// two-channel form computes, so results are bit-exact. The top byte is masked to zero like the scalar output.
void blendSpanNeon(std::uint32_t* dst, int n, std::uint32_t rgb, std::uint32_t a256) noexcept
{
    const uint8x16_t s8 = vreinterpretq_u8_u32(vdupq_n_u32(rgb));
    const uint16x8_t sa = vmulq_n_u16(vmovl_u8(vget_low_u8(s8)), static_cast<std::uint16_t>(a256));
    const auto inv = static_cast<std::uint16_t>(256 - a256);
    const uint32x4_t rgbMask = vdupq_n_u32(0x00FFFFFFu);
    int i = 0;
    for (; i + 4 <= n; i += 4)
    {
        const uint8x16_t d = vreinterpretq_u8_u32(vld1q_u32(dst + i));
        const uint16x8_t lo = vmlaq_n_u16(sa, vmovl_u8(vget_low_u8(d)), inv);
        const uint16x8_t hi = vmlaq_n_u16(sa, vmovl_u8(vget_high_u8(d)), inv);
        const uint8x16_t r = vcombine_u8(vshrn_n_u16(lo, 8), vshrn_n_u16(hi, 8));
        vst1q_u32(dst + i, vandq_u32(vreinterpretq_u32_u8(r), rgbMask));
    }
    for (; i < n; ++i)
        dst[i] = blendPixel(dst[i], rgb, a256);
}

// Eight pixels per step; steps whose eight coverage bytes are all zero (most of a glyph box) cost one load and a test.
void maskBlitNeon(std::uint32_t* dst, const std::uint8_t* cov, int n, std::uint32_t rgb, std::uint32_t alpha) noexcept
{
    const uint8x16_t s8 = vreinterpretq_u8_u32(vdupq_n_u32(rgb));
    const uint16x8_t s16 = vmovl_u8(vget_low_u8(s8)); // two pixels' channels
    const uint16x8_t k127 = vdupq_n_u16(127), k1 = vdupq_n_u16(1), k256 = vdupq_n_u16(256);
    const uint32x4_t rgbMask = vdupq_n_u32(0x00FFFFFFu);
    int i = 0;
    for (; i + 8 <= n; i += 8)
    {
        const uint8x8_t c8 = vld1_u8(cov + i);
        if (vget_lane_u64(vreinterpret_u64_u8(c8), 0) == 0)
            continue;
        uint16x8_t t = vmlaq_n_u16(k127, vmovl_u8(c8), static_cast<std::uint16_t>(alpha)); // cov * alpha + 127
        t = vshrq_n_u16(vaddq_u16(vaddq_u16(t, k1), vshrq_n_u16(t, 8)), 8);                // exact / 255 below 65536
        const uint16x8_t a = vaddq_u16(t, vshrq_n_u16(t, 7));                              // 255 -> 256
        const uint16x8_t inv = vsubq_u16(k256, a);
        const uint8x8_t zero8 = vceq_u8(c8, vdup_n_u8(0)); // lanes to keep unchanged
        for (int q = 0; q < 2; ++q)                        // 4 pixels per half
        {
            const uint16x4_t aq = q == 0 ? vget_low_u16(a) : vget_high_u16(a);
            const uint16x4_t iq = q == 0 ? vget_low_u16(inv) : vget_high_u16(inv);
            const uint16x4x2_t az = vzip_u16(aq, aq); // a0 a0 a1 a1 | a2 a2 a3 a3
            const uint16x4x2_t iz = vzip_u16(iq, iq);
            const uint16x4x2_t a01 = vzip_u16(az.val[0], az.val[0]); // a0 x4 | a1 x4
            const uint16x4x2_t a23 = vzip_u16(az.val[1], az.val[1]);
            const uint16x4x2_t i01 = vzip_u16(iz.val[0], iz.val[0]);
            const uint16x4x2_t i23 = vzip_u16(iz.val[1], iz.val[1]);
            std::uint32_t* p = dst + i + 4 * q;
            const uint32x4_t old = vld1q_u32(p);
            const uint8x16_t d = vreinterpretq_u8_u32(old);
            uint16x8_t lo = vmulq_u16(s16, vcombine_u16(a01.val[0], a01.val[1]));
            lo = vmlaq_u16(lo, vmovl_u8(vget_low_u8(d)), vcombine_u16(i01.val[0], i01.val[1]));
            uint16x8_t hi = vmulq_u16(s16, vcombine_u16(a23.val[0], a23.val[1]));
            hi = vmlaq_u16(hi, vmovl_u8(vget_high_u8(d)), vcombine_u16(i23.val[0], i23.val[1]));
            const uint32x4_t blended =
                vandq_u32(vreinterpretq_u32_u8(vcombine_u8(vshrn_n_u16(lo, 8), vshrn_n_u16(hi, 8))), rgbMask);
            // keep mask per pixel: widen the 4 zero-coverage bytes of this half to 32-bit lanes
            const uint8x8_t zq = q == 0 ? zero8 : vext_u8(zero8, zero8, 4);
            const uint16x4_t z16 = vget_low_u16(vmovl_u8(zq));
            const uint32x4_t keep = vtstq_u32(vmovl_u16(z16), vdupq_n_u32(0xFFu));
            vst1q_u32(p, vbslq_u32(keep, old, blended));
        }
    }
    maskBlitScalar(dst + i, cov + i, n - i, rgb, alpha);
}
#endif
} // namespace ui::simd
