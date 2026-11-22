// Single-precision fdlibm tanf, for the Phaser's parity maths (spec effect-shell.md FX-054, phaser.md
// "Test notes": the sweep corners tan(π·f/fs) are sensitive to the reference's libm tanf, which is not correctly
// rounded; research docs/effects/phaser.md, numerics.md, CC BY 4.0). Independent C++ port of the public fdlibm
// algorithm (s_tanf.c + k_tanf.c + e_rem_pio2f.c, the float versions glibc 2.13 on the device runs), written from the
// MIT transliteration tools/fx/fdlibm_tanf.py of the research (pinned commit 8901c97).
//
// fdlibm notice (must be preserved):
//   Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
//   Developed at SunPro, a Sun Microsystems, Inc. business.
//   Permission to use, copy, modify, and distribute this software is freely granted,
//   provided that this notice is preserved.
//   Conversion to float by Ian Lance Taylor, Cygnus Support.
//
// Every float operation is one IEEE float32 operation (never fused). Covers finite |x| < 3π/4 (the Phaser reaches
// |x| <= π·20000/44100 ≈ 1.42); larger or non-finite arguments fall back to std::tan in double, rounded once.
// Message-rate code (parameter changes and activation), never per sample.
#pragma once

#include "engine/fx/dsp/Numerics.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace engine::fx::dsp::fdlibm
{
namespace tanf_detail
{
inline std::int32_t w(float x) noexcept
{
    std::int32_t u;
    std::memcpy(&u, &x, sizeof u);
    return u;
}
inline float f(std::uint32_t u) noexcept
{
    float x;
    std::memcpy(&x, &u, sizeof x);
    return x;
}
inline float trunc12(float x) noexcept
{
    return f(static_cast<std::uint32_t>(w(x)) & 0xfffff000u);
}

// k_tanf.c
inline float kernel(float x, float y, int iy) noexcept
{
    APP_FX_NOFUSE
    const float one = 1.0f, pio4 = f(0x3f490fda), pio4lo = f(0x33222168);
    const float T[13] = {f(0x3eaaaaab), f(0x3e088889), f(0x3d5d0dd1), f(0x3cb327a4), f(0x3c11371f),
                         f(0x3b6b6916), f(0x3abede48), f(0x3a1a26c8), f(0x398137b9), f(0x38a3f445),
                         f(0x3895c07a), f(0xb79bae5f), f(0x37d95384)};
    const std::int32_t hx = w(x), ix = hx & 0x7fffffff;
    if (ix < 0x31800000) // |x| < 2^-28
    {
        if (static_cast<int>(x) == 0)
        {
            if ((ix | (iy + 1)) == 0)
                return one / (x < 0.0f ? -x : x);
            return iy == 1 ? x : -one / x;
        }
    }
    const bool big = ix >= 0x3f2ca140; // |x| >= 0.6744
    if (big)
    {
        if (hx < 0)
        {
            x = -x;
            y = -y;
        }
        const float z = pio4 - x, ww = pio4lo - y;
        x = z + ww;
        y = 0.0f;
    }
    float z = x * x;
    float ww = z * z;
    float r = T[1] + ww * (T[3] + ww * (T[5] + ww * (T[7] + ww * (T[9] + ww * T[11]))));
    float v = z * (T[2] + ww * (T[4] + ww * (T[6] + ww * (T[8] + ww * (T[10] + ww * T[12])))));
    float s = z * x;
    r = y + z * (s * (r + v) + y);
    r = r + T[0] * s;
    ww = x + r;
    if (big)
    {
        v = static_cast<float>(iy);
        const float sign = static_cast<float>(1 - ((hx >> 30) & 2));
        return sign * (v - 2.0f * (x - ((ww * ww) / (ww + v) - r)));
    }
    if (iy == 1)
        return ww;
    // compute −1/(x + r) accurately
    z = trunc12(ww);
    v = r - (z - x);
    const float a = -one / ww;
    const float t = trunc12(a);
    s = one + t * z;
    return t + a * (s + t * v);
}
} // namespace tanf_detail

// s_tanf.c (+ the |x| < 3π/4 part of e_rem_pio2f.c)
inline float tanf(float x) noexcept
{
    APP_FX_NOFUSE
    using tanf_detail::f;
    using tanf_detail::kernel;
    const std::int32_t hx = tanf_detail::w(x), ix = hx & 0x7fffffff;
    if (ix <= 0x3f490fda)
        return kernel(x, 0.0f, 1);
    if (ix >= 0x7f800000 || ix >= 0x4016cbe4) // NaN/Inf, or |x| >= 3π/4: outside the port's range
        return static_cast<float>(std::tan(static_cast<double>(x)));
    const float pio2_1 = f(0x3fc90f80), pio2_1t = f(0x37354443), pio2_2 = f(0x37354400), pio2_2t = f(0x2e85a308);
    float y0, y1;
    int n;
    if (ix <= 0x3f490fd8)
    {
        n = 0;
        y0 = x;
        y1 = 0.0f;
    }
    else if (hx > 0)
    {
        float z = x - pio2_1;
        if ((ix & 0xfffffff0) != 0x3fc90fd0)
        {
            y0 = z - pio2_1t;
            y1 = (z - y0) - pio2_1t;
        }
        else
        {
            z = z - pio2_2;
            y0 = z - pio2_2t;
            y1 = (z - y0) - pio2_2t;
        }
        n = 1;
    }
    else
    {
        float z = x + pio2_1;
        if ((ix & 0xfffffff0) != 0x3fc90fd0)
        {
            y0 = z + pio2_1t;
            y1 = (z - y0) + pio2_1t;
        }
        else
        {
            z = z + pio2_2;
            y0 = z + pio2_2t;
            y1 = (z - y0) + pio2_2t;
        }
        n = -1;
    }
    return kernel(y0, y1, 1 - ((n & 1) << 1));
}
} // namespace engine::fx::dsp::fdlibm
