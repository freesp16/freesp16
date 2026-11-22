// Single-precision fdlibm logf / log10f / powf, for the Filter and 2-band EQ step counts and the EQ's
// parity-maths frequency law (spec effect-shell.md FX-054; research docs/effects/parametric-eq.md "Math library",
// multimode-filter.md "Glide and smoothing", CC BY 4.0). Independent C++ port of the public fdlibm algorithms
// (e_logf.c, e_log10f.c, e_powf.c, the float versions glibc used up to 2.27 — so glibc 2.13 on the device runs exactly
// these), written from the MIT transliteration tools/fx/fdlibm_powf.py of the research (pinned commit 8901c97).
//
// fdlibm notice (must be preserved):
//   Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
//   Developed at SunPro, a Sun Microsystems, Inc. business.
//   Permission to use, copy, modify, and distribute this software is freely granted,
//   provided that this notice is preserved.
//   Conversion to float by Ian Lance Taylor, Cygnus Support.
//
// Why a port (and not std::log10f): the reference's glide step counts are round/trunc(|log10f(ratio) / log10f(m)|) + 1
// and must match exactly; a host libm's log10f is not the fdlibm one, so a 1-ulp
// difference could flip a count. The step counts therefore always use this port (message-rate code, called only
// on parameter changes); coefficients use std:: maths in double unless parity maths is on (FX-054).
//
// Every float operation is one IEEE float32 operation (no contraction: products are never fused — the engine's host
// flags are -ffp-contract=off, and ARMv7 VFP/NEON have no fused multiply-add). Only finite positive arguments are
// handled exactly (the effect code reaches no others); special values fall back to std::.
#pragma once

#include "engine/fx/dsp/Numerics.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace engine::fx::dsp::fdlibm
{
namespace detail
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
} // namespace detail

// e_logf.c
inline float logf(float x) noexcept
{
    APP_FX_NOFUSE
    using detail::f;
    const float ln2_hi = f(0x3f317180), ln2_lo = f(0x3717f7d1);
    const float Lg1 = f(0x3f2aaaab), Lg2 = f(0x3ecccccd), Lg3 = f(0x3e924925), Lg4 = f(0x3e638e29);
    const float Lg5 = f(0x3e3a3325), Lg6 = f(0x3e1cd04f), Lg7 = f(0x3e178897);
    const float third = 0.33333333333333333f;
    std::int32_t ix = detail::w(x);
    if (ix < 0x00800000 || ix >= 0x7f800000)
        return std::log(x); // zero, negative, subnormal, inf, NaN: not reached by the effects
    std::int32_t k = (ix >> 23) - 127;
    ix &= 0x007fffff;
    std::int32_t i = (ix + (0x95f64 << 3)) & 0x800000;
    x = f(static_cast<std::uint32_t>(ix | (i ^ 0x3f800000)));
    k += i >> 23;
    const float fm = x - 1.0f;
    if ((0x007fffff & (15 + ix)) < 16)
    {
        if (fm == 0.0f)
        {
            if (k == 0)
                return 0.0f;
            const float dk = static_cast<float>(k);
            return dk * ln2_hi + dk * ln2_lo;
        }
        const float R = (fm * fm) * (0.5f - third * fm);
        if (k == 0)
            return fm - R;
        const float dk = static_cast<float>(k);
        return dk * ln2_hi - ((R - dk * ln2_lo) - fm);
    }
    const float s = fm / (2.0f + fm);
    const float dk = static_cast<float>(k);
    const float z = s * s;
    i = ix - (0x6147a << 3);
    const float ww = z * z;
    const std::int32_t j = (0x6b851 << 3) - ix;
    const float t1 = ww * (Lg2 + ww * (Lg4 + ww * Lg6));
    const float t2 = z * (Lg1 + ww * (Lg3 + ww * (Lg5 + ww * Lg7)));
    i |= j;
    const float R = t2 + t1;
    if (i > 0)
    {
        const float hfsq = (0.5f * fm) * fm;
        if (k == 0)
            return fm - (hfsq - s * (hfsq + R));
        return dk * ln2_hi - ((hfsq - (s * (hfsq + R) + dk * ln2_lo)) - fm);
    }
    if (k == 0)
        return fm - s * (fm - R);
    return dk * ln2_hi - ((s * (fm - R) - dk * ln2_lo) - fm);
}

// e_log10f.c
inline float log10f(float x) noexcept
{
    APP_FX_NOFUSE
    using detail::f;
    const float ivln10 = f(0x3ede5bd9), log10_2hi = f(0x3e9a2080), log10_2lo = f(0x355427db);
    std::int32_t hx = detail::w(x);
    if (hx < 0x00800000 || hx >= 0x7f800000)
        return std::log10(x); // not reached by the effects
    const std::int32_t k = (hx >> 23) - 127;
    const std::int32_t i = k < 0 ? 1 : 0;
    hx = (hx & 0x007fffff) | ((0x7f - i) << 23);
    const float y = static_cast<float>(k + i);
    const float z = y * log10_2lo + ivln10 * logf(f(static_cast<std::uint32_t>(hx)));
    return z + y * log10_2hi;
}

// e_powf.c for finite x > 0 and finite y (the historical t_h constant 0x0040000, as glibc 2.13 and the research model)
inline float powf(float x, float y) noexcept
{
    APP_FX_NOFUSE
    using detail::f;
    using detail::trunc12;
    using detail::w;
    const float bp[2] = {1.0f, 1.5f};
    const float dp_h[2] = {0.0f, f(0x3f15c000)};
    const float dp_l[2] = {0.0f, f(0x35d1cfdc)};
    const float two24 = f(0x4b800000);
    const float L1 = f(0x3f19999a), L2 = f(0x3edb6db7), L3 = f(0x3eaaaaab);
    const float L4 = f(0x3e8ba305), L5 = f(0x3e6c3255), L6 = f(0x3e53f142);
    const float P1 = f(0x3e2aaaab), P2 = f(0xbb360b61), P3 = f(0x388ab355);
    const float P4 = f(0xb5ddea0e), P5 = f(0x3331bb4c);
    const float lg2 = f(0x3f317218), lg2_h = f(0x3f317200), lg2_l = f(0x35bfbe8c);
    const float cp = f(0x3f76384f), cp_h = f(0x3f763800), cp_l = f(0x369dc3a0);
    constexpr std::int32_t kThAdd = 0x0040000;

    const std::int32_t hx = w(x), hy = w(y);
    std::int32_t ix = hx & 0x7fffffff;
    const std::int32_t iy = hy & 0x7fffffff;
    if (iy == 0 || x == 1.0f)
        return 1.0f;
    if (ix > 0x7f800000 || iy > 0x7f800000 || hx < 0 || iy >= 0x7f800000 || ix >= 0x7f800000 || ix == 0)
        return std::pow(x, y); // special values: not reached by the effects
    if (iy == 0x3f800000)
        return hy >= 0 ? x : 1.0f / x;
    if (hy == 0x40000000)
        return x * x;
    if (hy == 0x3f000000)
        return std::sqrt(x);
    float ax = x;
    if (ix == 0x3f800000)
        return 1.0f;
    if (iy > 0x4d000000)
        return std::pow(x, y); // |y| > 2^27: not reached
    std::int32_t n = 0;
    if (ix < 0x00800000)
    {
        ax = ax * two24;
        n -= 24;
        ix = w(ax);
    }
    n += (ix >> 23) - 0x7f;
    std::int32_t j = ix & 0x007fffff;
    ix = j | 0x3f800000;
    int k;
    if (j <= 0x1cc471)
        k = 0;
    else if (j < 0x5db3d7)
        k = 1;
    else
    {
        k = 0;
        n += 1;
        ix -= 0x00800000;
    }
    ax = f(static_cast<std::uint32_t>(ix));
    float u = ax - bp[k];
    float v = 1.0f / (ax + bp[k]);
    const float s = u * v;
    const float s_h = trunc12(s);
    float t_h = f(static_cast<std::uint32_t>(((ix >> 1) | 0x20000000) + kThAdd + (k << 21)));
    float t_l = ax - (t_h - bp[k]);
    const float s_l = v * ((u - s_h * t_h) - s_h * t_l);
    float s2 = s * s;
    float poly = L5 + s2 * L6;
    poly = L4 + s2 * poly;
    poly = L3 + s2 * poly;
    poly = L2 + s2 * poly;
    poly = L1 + s2 * poly;
    float r = (s2 * s2) * poly;
    r = r + s_l * (s_h + s);
    s2 = s_h * s_h;
    t_h = trunc12((3.0f + s2) + r);
    t_l = r - ((t_h - 3.0f) - s2);
    u = s_h * t_h;
    v = s_l * t_h + t_l * s;
    float p_h = trunc12(u + v);
    float p_l = v - (p_h - u);
    const float z_h = cp_h * p_h;
    const float z_l = (cp_l * p_h + p_l * cp) + dp_l[k];
    float t = static_cast<float>(n);
    const float t1 = trunc12(((z_h + z_l) + dp_h[k]) + t);
    const float t2 = z_l - (((t1 - t) - dp_h[k]) - z_h);
    const float y1 = trunc12(y);
    p_l = (y - y1) * t1 + y * t2;
    p_h = y1 * t1;
    float z = p_l + p_h;
    j = w(z);
    if (j > 0x43000000 || (j & 0x7fffffff) > 0x43160000 || j == 0x43000000 ||
        j == static_cast<std::int32_t>(0xc3160000u))
        return std::pow(x, y); // overflow / underflow edges: not reached
    const std::int32_t i = j & 0x7fffffff;
    k = (i >> 23) - 0x7f;
    n = 0;
    if (i > 0x3f000000)
    {
        n = j + (0x00800000 >> (k + 1));
        k = ((n & 0x7fffffff) >> 23) - 0x7f;
        t = f(static_cast<std::uint32_t>(n) & ~(0x007fffffu >> k));
        n = ((n & 0x007fffff) | 0x00800000) >> (23 - k);
        if (j < 0)
            n = -n;
        p_h = p_h - t;
    }
    t = trunc12(p_l + p_h);
    u = t * lg2_h;
    v = (p_l - (t - p_h)) * lg2 + t * lg2_l;
    z = u + v;
    const float ww = v - (z - u);
    t = z * z;
    float q = P4 + t * P5;
    q = P3 + t * q;
    q = P2 + t * q;
    q = P1 + t * q;
    const float tt1 = z - t * q;
    r = (z * tt1) / (tt1 - 2.0f) - (ww + z * ww);
    z = 1.0f - (r - z);
    const std::int64_t jj = static_cast<std::int64_t>(w(z)) + static_cast<std::int64_t>(n) * (1 << 23);
    if ((jj >> 23) <= 0)
        return std::ldexp(z, n);
    return f(static_cast<std::uint32_t>(jj & 0xffffffff));
}
} // namespace engine::fx::dsp::fdlibm
