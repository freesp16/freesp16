// Building blocks of the Vintage Pitch Shifter (engine/fx/effects/VintagePitch.*): a model of a late-1970s
// digital harmonizer (10-bit ramp A/D, 1K-word RAM window, two read taps with a staircase crossfade, multiplying
// D/As, 2:1 / 1:2 compander around the converters). Our own design, ported from a standalone emulation of the
// hardware written for this project; nothing here is reconstructed from any instrument firmware.
//
//   V2<Neon>   the lane pair the whole per-sample chain runs on: L in lane 0, R in lane 1. V2<true> wraps a NEON
//              float32x2_t, V2<false> is the scalar reference with the same operations in the same order (products
//              never fused: APP_FX_NOFUSE; no division, no libm: floor, log2 and exp2 are integer/polynomial), so the
//              two are bit-equal on normal inputs (tests/unit/fx/test_fx_vpitch.cpp, run under qemu-arm by
//              scripts/neon-tests.sh).
//   Rng2       one xorshift32 per lane (A/D gating noise).
//   Svf2       TPT state-variable low-pass (anti-alias filter, reconstruction filter).
//   Shelf2     first-order shelf (pre-/de-emphasis), TDF-II.
//   Dc2        one-pole high-pass (coupling capacitors).
//   Comp2      compressor: emphasis, feedback mean-square detector, 2:1 about the unity point, VCA asymmetry.
//   Expand2    expander: coupling, feed-forward detector (the compressor's mirror), 1:2, de-emphasis.
//   Out2       output buffer: coupling, slew limit, C1 knee to the rails.
//   Blep2      the D/A's zero-order hold rendered at the host rate with 2-point polyBLEP residuals.
//
// Every block takes host-rate samples; coefficients are set at control rate (never per sample).
#pragma once

#include "engine/fx/dsp/Numerics.h"

#include <cmath>
#include <cstdint>
#include <cstring>

#if APP_FX_HAVE_NEON
#include <arm_neon.h>
#endif

namespace engine::fx::dsp::vpitch
{
// Hardware constants (the emulation's tags: [M] manual-level facts, [I] inferred/calibrated).
constexpr float kFrefHz = 33488.0f;    // A/D frame clock: 8 × 4186 Hz [M]
constexpr float kFinGridHz = 12.85e6f; // read-clock re-timing grid (51.4 MHz / 4) [I]
constexpr int kAdcMid = 512;           // 10-bit offset binary [M]
constexpr int kWindow = 1024;          // pointer span: 1024 words = 30.6 ms [M]
constexpr int kRamWords = 8192;        // window + 120 ms of ADD'L DELAY at FREF, power of two
constexpr std::uint32_t kRamMask = kRamWords - 1;
constexpr int kEchoWords = 1024;   // tap B = tap A, 1024 read events later [M]
constexpr int kXfadeLength = 1024; // CD4020 counts per crossfade window [M]
constexpr int kXfadeHalf = kXfadeLength / 2;
constexpr float kRatioMin = 0.5f, kRatioMax = 2.0f; // VCO floor / two reads per frame [M]
constexpr float kAaEdgeHz = 12000.0f;               // 4-pole anti-alias filter, peaked [M]/[I]
constexpr float kReconHz = 15000.0f;                // 2-pole reconstruction filter [I]
constexpr float kReconQ = 0.85f;
constexpr float kEmphasisZeroHz = 1500.0f; // compander pre-emphasis shelf start [I]
constexpr float kEmphasisDb = 10.0f;
constexpr float kCompRefLog2 = -1.0f; // compander unity point: a full-scale sine (mean square 0.5)
constexpr float kCompMinLog2 = -24.0f * 0.16609640474f, kCompMaxLog2 = 45.0f * 0.16609640474f;
constexpr float kExpMinLog2 = -60.0f * 0.16609640474f, kExpMaxLog2 = 24.0f * 0.16609640474f;
constexpr float kVcaAsymComp = 0.0020f, kVcaAsymExp = -0.0015f; // residual 2nd harmonic of the VCAs [I]
constexpr float kAttackMs = 2.0f, kReleaseMs = 40.0f;
constexpr float kExpCouplingHz = 8.0f, kOutCouplingHz = 4.5f;
constexpr float kOutRail = 13.0f / 8.0f;       // ±13 V rails over the 8 V peak full scale
constexpr float kOutSlewFsPerUs = 0.6f / 8.0f; // LM301A slew, full scale per µs [I]

// ------------------------------------------------------------------------------------------------ the lane pair

template <bool Neon> struct V2;
template <bool Neon> struct M2; // lane mask

template <> struct M2<false>
{
    bool a = false, b = false;
    bool any() const noexcept { return a || b; }
    friend M2 operator|(M2 x, M2 y) noexcept { return {x.a || y.a, x.b || y.b}; }
};
template <> struct V2<false>
{
    float a = 0.0f, b = 0.0f;

    static V2 dup(float x) noexcept { return {x, x}; }
    static V2 make(float l, float r) noexcept { return {l, r}; }
    static V2 load(const float* l, const float* r, int i) noexcept { return {l[i], r[i]}; }
    void store(float* l, float* r, int i) const noexcept
    {
        l[i] = a;
        r[i] = b;
    }
    float lane0() const noexcept { return a; }
    float lane1() const noexcept { return b; }

    friend V2 operator+(V2 x, V2 y) noexcept { APP_FX_NOFUSE return {x.a + y.a, x.b + y.b}; }
    friend V2 operator-(V2 x, V2 y) noexcept { APP_FX_NOFUSE return {x.a - y.a, x.b - y.b}; }
    friend V2 operator*(V2 x, V2 y) noexcept { APP_FX_NOFUSE return {x.a * y.a, x.b * y.b}; }
    friend V2 operator*(V2 x, float g) noexcept { APP_FX_NOFUSE return {x.a * g, x.b * g}; }
    friend V2 vmin(V2 x, V2 y) noexcept { return {x.a < y.a ? x.a : y.a, x.b < y.b ? x.b : y.b}; }
    friend V2 vmax(V2 x, V2 y) noexcept { return {x.a > y.a ? x.a : y.a, x.b > y.b ? x.b : y.b}; }
    friend V2 vabs(V2 x) noexcept { return {x.a < 0.0f ? -x.a : x.a, x.b < 0.0f ? -x.b : x.b}; }
    friend M2<false> gt(V2 x, V2 y) noexcept { return {x.a > y.a, x.b > y.b}; }
    friend M2<false> lt(V2 x, V2 y) noexcept { return {x.a < y.a, x.b < y.b}; }
    friend V2 select(M2<false> m, V2 x, V2 y) noexcept { return {m.a ? x.a : y.a, m.b ? x.b : y.b}; }
    friend V2 copySign(V2 mag, V2 sign) noexcept // |mag| with the sign bit of `sign`
    {
        return {bits(mag.a, sign.a), bits(mag.b, sign.b)};
    }
    // floor() through truncation (vcvt semantics, |x| < 2^31) and a −1 where the truncation rounded up.
    friend V2 vfloor(V2 x) noexcept { return {floor1(x.a), floor1(x.b)}; }
    // Truncated integer lanes of a non-negative integral value, ANDed with `mask` (the A/D's dead LSBs).
    void toCodes(std::int32_t mask, std::int16_t& l, std::int16_t& r) const noexcept
    {
        l = static_cast<std::int16_t>(static_cast<std::int32_t>(a) & mask);
        r = static_cast<std::int16_t>(static_cast<std::int32_t>(b) & mask);
    }
    friend V2 fastLog2(V2 x) noexcept { return {log2one(x.a), log2one(x.b)}; }
    friend V2 fastExp2(V2 x) noexcept { return {exp2one(x.a), exp2one(x.b)}; }

    static float floor1(float x) noexcept
    {
        float t = static_cast<float>(static_cast<std::int32_t>(x));
        if (t > x)
            t = t - 1.0f;
        return t;
    }
    // log2(x), x > 0: exponent + a 4th-order mantissa fit (max abs error ≈ 2e-4)
    static float log2one(float x) noexcept
    {
        APP_FX_NOFUSE
        std::uint32_t i;
        std::memcpy(&i, &x, 4);
        const float e = static_cast<float>(static_cast<std::int32_t>((i >> 23) & 0xFFu) - 127);
        i = (i & 0x007FFFFFu) | 0x3F800000u;
        float m;
        std::memcpy(&m, &i, 4);
        float p = m * -0.07915037f;
        p = p + 0.62881573f;
        p = p * m;
        p = p - 2.08106020f;
        p = p * m;
        p = p + 4.02837277f;
        p = p * m;
        p = p - 2.49677377f;
        return e + p;
    }
    // 2^x on [−60, 60] (max rel error ≈ 7e-6)
    static float exp2one(float x) noexcept
    {
        APP_FX_NOFUSE
        x = x < -60.0f ? -60.0f : (x > 60.0f ? 60.0f : x);
        const float fi = floor1(x);
        const float f = x - fi;
        float p = f * 0.01367653f;
        p = p + 0.05166688f;
        p = p * f;
        p = p + 0.24171026f;
        p = p * f;
        p = p + 0.69293129f;
        p = p * f;
        p = p + 1.00000728f;
        const std::uint32_t bits = static_cast<std::uint32_t>(static_cast<std::int32_t>(fi) + 127) << 23;
        float s;
        std::memcpy(&s, &bits, 4);
        return p * s;
    }
    static float bits(float mag, float sign) noexcept
    {
        std::uint32_t m, s;
        std::memcpy(&m, &mag, 4);
        std::memcpy(&s, &sign, 4);
        m = (m & 0x7FFFFFFFu) | (s & 0x80000000u);
        float r;
        std::memcpy(&r, &m, 4);
        return r;
    }
};

#if APP_FX_HAVE_NEON
template <> struct M2<true>
{
    uint32x2_t m;
    bool any() const noexcept { return (vget_lane_u32(m, 0) | vget_lane_u32(m, 1)) != 0u; }
    friend M2 operator|(M2 x, M2 y) noexcept { return {vorr_u32(x.m, y.m)}; }
};
template <> struct V2<true>
{
    float32x2_t v;

    static V2 dup(float x) noexcept { return {vdup_n_f32(x)}; }
    static V2 make(float l, float r) noexcept { return {vset_lane_f32(r, vdup_n_f32(l), 1)}; }
    static V2 load(const float* l, const float* r, int i) noexcept { return make(l[i], r[i]); }
    void store(float* l, float* r, int i) const noexcept
    {
        l[i] = vget_lane_f32(v, 0);
        r[i] = vget_lane_f32(v, 1);
    }
    float lane0() const noexcept { return vget_lane_f32(v, 0); }
    float lane1() const noexcept { return vget_lane_f32(v, 1); }

    friend V2 operator+(V2 x, V2 y) noexcept { APP_FX_NOFUSE return {vadd_f32(x.v, y.v)}; }
    friend V2 operator-(V2 x, V2 y) noexcept { APP_FX_NOFUSE return {vsub_f32(x.v, y.v)}; }
    friend V2 operator*(V2 x, V2 y) noexcept { APP_FX_NOFUSE return {vmul_f32(x.v, y.v)}; }
    friend V2 operator*(V2 x, float g) noexcept { APP_FX_NOFUSE return {vmul_n_f32(x.v, g)}; }
    friend V2 vmin(V2 x, V2 y) noexcept { return {vmin_f32(x.v, y.v)}; }
    friend V2 vmax(V2 x, V2 y) noexcept { return {vmax_f32(x.v, y.v)}; }
    friend V2 vabs(V2 x) noexcept { return {vabs_f32(x.v)}; }
    friend M2<true> gt(V2 x, V2 y) noexcept { return {vcgt_f32(x.v, y.v)}; }
    friend M2<true> lt(V2 x, V2 y) noexcept { return {vclt_f32(x.v, y.v)}; }
    friend V2 select(M2<true> m, V2 x, V2 y) noexcept { return {vbsl_f32(m.m, x.v, y.v)}; }
    friend V2 copySign(V2 mag, V2 sign) noexcept
    {
        return {vbsl_f32(vdup_n_u32(0x80000000u), sign.v, vabs_f32(mag.v))};
    }
    friend V2 vfloor(V2 x) noexcept
    {
        const float32x2_t t = vcvt_f32_s32(vcvt_s32_f32(x.v));
        const uint32x2_t up = vcgt_f32(t, x.v);
        return {vsub_f32(t, vbsl_f32(up, vdup_n_f32(1.0f), vdup_n_f32(0.0f)))};
    }
    void toCodes(std::int32_t mask, std::int16_t& l, std::int16_t& r) const noexcept
    {
        const int32x2_t c = vand_s32(vcvt_s32_f32(v), vdup_n_s32(mask));
        l = static_cast<std::int16_t>(vget_lane_s32(c, 0));
        r = static_cast<std::int16_t>(vget_lane_s32(c, 1));
    }
    friend V2 fastLog2(V2 x) noexcept
    {
        const uint32x2_t i = vreinterpret_u32_f32(x.v);
        const int32x2_t ex =
            vsub_s32(vreinterpret_s32_u32(vand_u32(vshr_n_u32(i, 23), vdup_n_u32(0xFFu))), vdup_n_s32(127));
        const float32x2_t e = vcvt_f32_s32(ex);
        const float32x2_t m =
            vreinterpret_f32_u32(vorr_u32(vand_u32(i, vdup_n_u32(0x007FFFFFu)), vdup_n_u32(0x3F800000u)));
        float32x2_t p = vmul_n_f32(m, -0.07915037f);
        p = vadd_f32(p, vdup_n_f32(0.62881573f));
        p = vmul_f32(p, m);
        p = vsub_f32(p, vdup_n_f32(2.08106020f));
        p = vmul_f32(p, m);
        p = vadd_f32(p, vdup_n_f32(4.02837277f));
        p = vmul_f32(p, m);
        p = vsub_f32(p, vdup_n_f32(2.49677377f));
        return {vadd_f32(e, p)};
    }
    friend V2 fastExp2(V2 x) noexcept
    {
        const float32x2_t c = vmax_f32(vmin_f32(x.v, vdup_n_f32(60.0f)), vdup_n_f32(-60.0f));
        const float32x2_t fi = vfloor(V2{c}).v;
        const float32x2_t f = vsub_f32(c, fi);
        float32x2_t p = vmul_n_f32(f, 0.01367653f);
        p = vadd_f32(p, vdup_n_f32(0.05166688f));
        p = vmul_f32(p, f);
        p = vadd_f32(p, vdup_n_f32(0.24171026f));
        p = vmul_f32(p, f);
        p = vadd_f32(p, vdup_n_f32(0.69293129f));
        p = vmul_f32(p, f);
        p = vadd_f32(p, vdup_n_f32(1.00000728f));
        const int32x2_t e = vshl_n_s32(vadd_s32(vcvt_s32_f32(fi), vdup_n_s32(127)), 23);
        return {vmul_f32(p, vreinterpret_f32_s32(e))};
    }
};
#endif

// clamp(x, lo, hi) as vmax(vmin(x, hi), lo) — the same order in both lane types.
template <bool Neon> inline V2<Neon> clamp2(V2<Neon> x, float lo, float hi) noexcept
{
    return vmax(vmin(x, V2<Neon>::dup(hi)), V2<Neon>::dup(lo));
}

// ------------------------------------------------------------------------------------------------ noise

// xorshift32 per lane (the lanes start from different seeds); bipolar() = int32 · 2^-31.
template <bool Neon> struct Rng2
{
    std::uint32_t s[2] = {0x9E3779B9u, 0x7F4A7C15u};
    void seed(std::uint32_t l, std::uint32_t r) noexcept
    {
        s[0] = l != 0u ? l : 0x9E3779B9u;
        s[1] = r != 0u ? r : 0x7F4A7C15u;
    }
    V2<Neon> bipolar() noexcept
    {
        float o[2];
        for (int k = 0; k < 2; ++k)
        {
            std::uint32_t x = s[k];
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            s[k] = x;
            o[k] = static_cast<float>(static_cast<std::int32_t>(x)) * 4.656612873e-10f;
        }
        return V2<Neon>::make(o[0], o[1]);
    }
};

// Scalar xorshift32 (shared control: VCO drift and read-clock jitter).
struct Rng
{
    std::uint32_t s = 0x9E3779B9u;
    void seed(std::uint32_t v) noexcept { s = v != 0u ? v : 0x9E3779B9u; }
    std::uint32_t next() noexcept
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    float bipolar() noexcept { return static_cast<float>(static_cast<std::int32_t>(next())) * 4.656612873e-10f; }
};

// ------------------------------------------------------------------------------------------------ filters

struct SvfCoefs // TPT state-variable low-pass (Zavalishin), set at control rate
{
    float a1 = 1.0f, a2 = 0.0f, a3 = 0.0f;
    void setup(double fs, double fcHz, double q) noexcept
    {
        const double fc = fcHz < 10.0 ? 10.0 : (fcHz > 0.49 * fs ? 0.49 * fs : fcHz);
        const double g = std::tan(3.141592653589793 * fc / fs);
        const double k = 1.0 / q;
        const double b1 = 1.0 / (1.0 + g * (g + k));
        a1 = static_cast<float>(b1);
        a2 = static_cast<float>(g * b1);
        a3 = static_cast<float>(g * g * b1);
    }
};

template <bool Neon> struct Svf2
{
    V2<Neon> ic1 = V2<Neon>::dup(0.0f), ic2 = V2<Neon>::dup(0.0f);
    void reset() noexcept { ic1 = ic2 = V2<Neon>::dup(0.0f); }
    V2<Neon> process(V2<Neon> x, const SvfCoefs& c) noexcept
    {
        APP_FX_NOFUSE
        const V2<Neon> v3 = x - ic2;
        const V2<Neon> v1 = ic1 * c.a1 + v3 * c.a2;
        const V2<Neon> v2 = ic2 + ic1 * c.a2 + v3 * c.a3;
        ic1 = v1 + v1 - ic1;
        ic2 = v2 + v2 - ic2;
        return v2;
    }
};

struct ShelfCoefs // H(s) = (1 + s/wz) / (1 + s/wp), bilinear with pre-warping
{
    float b0 = 1.0f, b1 = 0.0f, a1 = 0.0f;
    void setup(double fs, double fzHz, double fpHz) noexcept
    {
        const double K = 2.0 * fs;
        const auto warp = [&](double f)
        {
            f = f < 1.0 ? 1.0 : (f > 0.45 * fs ? 0.45 * fs : f);
            return K * std::tan(3.141592653589793 * f / fs);
        };
        const double wz = warp(fzHz), wp = warp(fpHz);
        const double a0 = 1.0 + K / wp;
        b0 = static_cast<float>((1.0 + K / wz) / a0);
        b1 = static_cast<float>((1.0 - K / wz) / a0);
        a1 = static_cast<float>((1.0 - K / wp) / a0);
    }
};

template <bool Neon> struct Shelf2
{
    V2<Neon> z = V2<Neon>::dup(0.0f);
    void reset() noexcept { z = V2<Neon>::dup(0.0f); }
    V2<Neon> process(V2<Neon> x, const ShelfCoefs& c) noexcept
    {
        APP_FX_NOFUSE
        const V2<Neon> y = x * c.b0 + z;
        z = x * c.b1 - y * c.a1;
        return y;
    }
};

template <bool Neon> struct Dc2 // one-pole high-pass: y = x − x1 + R·y1
{
    V2<Neon> x1 = V2<Neon>::dup(0.0f), y1 = V2<Neon>::dup(0.0f);
    void reset() noexcept { x1 = y1 = V2<Neon>::dup(0.0f); }
    V2<Neon> process(V2<Neon> x, float R) noexcept
    {
        APP_FX_NOFUSE
        const V2<Neon> y = (x - x1) + y1 * R;
        x1 = x;
        y1 = y;
        return y;
    }
};

// ------------------------------------------------------------------------------------------------ compander

struct CompanderCoefs
{
    float att = 0.01f, rel = 0.001f; // detector one-pole coefficients
    ShelfCoefs emph, deemph;
    float expCouplingR = 0.999f;
    float slope1 = 1.0f, slope2 = 1.0f; // expander gain law × (1 + mismatch), lane L / lane R
};

// The compander's VCA gain is a function of its detector only (2 ms attack, 40 ms release), so it is evaluated once
// per kGainQuad samples (log2 → exp2, the dear part) and ramped linearly to the new value over the quad: the
// per-sample work is the emphasis, the VCA multiply and the detector. Both sides use the same quad and the same lag,
// so a pure delay between them still reconstructs the input.
constexpr int kGainQuad = 4;

// Mean-square detector + VCA, compressor side (the detector listens to the VCA's own output: attack lag). The VCA's
// output stage clips at the ±13 V rails (kOutRail of the A/D's full scale) before the detector hears it: after
// silence the gain is at its +45 dB lift for the few samples the feedback detector lags, and an unclipped overshoot of
// 30× full scale would leave the compressor's detector hundreds of times above the expander's (which only sees the
// A/D's clip) — every hit after a pause would come out 6–9 dB low. Rail-clipped, the two detectors disagree by
// < 2 dB for a few samples.
template <bool Neon> struct Comp2
{
    Shelf2<Neon> emph;
    V2<Neon> p = V2<Neon>::dup(1e-6f);
    V2<Neon> g = gainOf(V2<Neon>::dup(1e-6f)), dg = V2<Neon>::dup(0.0f);
    static V2<Neon> gainOf(V2<Neon> p) noexcept
    {
        APP_FX_NOFUSE
        using V = V2<Neon>;
        return fastExp2(
            clamp2((V::dup(kCompRefLog2) - fastLog2(p + V::dup(1e-12f))) * 0.5f, kCompMinLog2, kCompMaxLog2));
    }
    void reset() noexcept
    {
        emph.reset();
        p = V2<Neon>::dup(1e-6f);
        g = gainOf(p);
        dg = V2<Neon>::dup(0.0f);
    }
    void quad() noexcept // once per kGainQuad samples, before the first
    {
        APP_FX_NOFUSE
        dg = (gainOf(p) - g) * (1.0f / kGainQuad);
    }
    V2<Neon> process(V2<Neon> x, const CompanderCoefs& c) noexcept
    {
        APP_FX_NOFUSE
        using V = V2<Neon>;
        const V xe = emph.process(x, c.emph);
        g = g + dg;
        V y = xe * g;
        y = y + (y * y) * kVcaAsymComp;
        y = clamp2(y, -kOutRail, kOutRail);
        const V y2 = y * y;
        const V k = select(gt(y2, p), V::dup(c.att), V::dup(c.rel));
        p = p + k * (y2 - p);
        return y;
    }
};

// Expander: detector on its input (the compressed signal), lagging like the compressor's, so a pure delay between
// the two reconstructs the input; pitch shift, splices and clipping in between mistrack naturally.
template <bool Neon> struct Expand2
{
    Dc2<Neon> coupling;
    Shelf2<Neon> deemph;
    V2<Neon> p = V2<Neon>::dup(1e-6f);
    V2<Neon> g = V2<Neon>::dup(1.0f), dg = V2<Neon>::dup(0.0f);
    static V2<Neon> gainOf(V2<Neon> p, const CompanderCoefs& k) noexcept
    {
        APP_FX_NOFUSE
        using V = V2<Neon>;
        return fastExp2(
            clamp2((fastLog2(p + V::dup(1e-12f)) - V::dup(kCompRefLog2)) * V::make(0.5f * k.slope1, 0.5f * k.slope2),
                   kExpMinLog2, kExpMaxLog2));
    }
    void reset(const CompanderCoefs& k) noexcept
    {
        coupling.reset();
        deemph.reset();
        p = V2<Neon>::dup(1e-6f);
        g = gainOf(p, k);
        dg = V2<Neon>::dup(0.0f);
    }
    void quad(const CompanderCoefs& k) noexcept
    {
        APP_FX_NOFUSE
        dg = (gainOf(p, k) - g) * (1.0f / kGainQuad);
    }
    V2<Neon> process(V2<Neon> c, const CompanderCoefs& k) noexcept
    {
        APP_FX_NOFUSE
        using V = V2<Neon>;
        c = coupling.process(c, k.expCouplingR);
        const V c2 = c * c;
        const V a = select(gt(c2, p), V::dup(k.att), V::dup(k.rel));
        p = p + a * (c2 - p);
        g = g + dg;
        V y = c * g;
        y = y + (y * y) * kVcaAsymExp;
        return deemph.process(y, k.deemph);
    }
};

// ------------------------------------------------------------------------------------------------ output stage

// Coupling capacitor, LM301A slew limit, then a C1 knee to the rails (division-free): linear to 0.8 of the rail,
// y = a − 1.25·(a − 0.8)² up to 1.2, the rail above.
template <bool Neon> struct Out2
{
    Dc2<Neon> coupling;
    V2<Neon> y = V2<Neon>::dup(0.0f);
    void reset() noexcept
    {
        coupling.reset();
        y = V2<Neon>::dup(0.0f);
    }
    V2<Neon> process(V2<Neon> x, float couplingR, float maxStep) noexcept
    {
        APP_FX_NOFUSE
        using V = V2<Neon>;
        const V d = clamp2(coupling.process(x, couplingR) - y, -maxStep, maxStep);
        y = y + d;
        const V a = vabs(y * (1.0f / kOutRail));
        const V over = vmax(a - V::dup(0.8f), V::dup(0.0f));
        const V k = vmin(over, V::dup(0.4f));
        const V knee = vmin(a, V::dup(1.2f)) - (k * k) * 1.25f;
        return copySign(knee, y) * kOutRail;
    }
};

// ------------------------------------------------------------------------------------------------ D/A rendering

// Zero-order hold of a D/A updated at asynchronous instants t ∈ [0, 1) of the host sample, rendered with 2-point
// polyBLEP residuals: the staircase's own images below the host Nyquist stay (they are the sound of a pitched-down
// harmonizer); only the extra aliasing of a naive host-rate hold is removed.
template <bool Neon> struct Blep2
{
    V2<Neon> level = V2<Neon>::dup(0.0f), pending = V2<Neon>::dup(0.0f), acc = V2<Neon>::dup(0.0f);
    void reset() noexcept { level = pending = acc = V2<Neon>::dup(0.0f); }
    void begin() noexcept
    {
        acc = level + pending;
        pending = V2<Neon>::dup(0.0f);
    }
    void step(V2<Neon> next, float t) noexcept
    {
        APP_FX_NOFUSE
        const float u = 1.0f - t;
        const float cu = (u * u) * 0.5f, ct = (t * t) * 0.5f;
        const V2<Neon> d = next - level;
        acc = acc + d * cu;
        pending = pending - d * ct;
        level = next;
    }
};

// 4-point Hermite between x0 and x1 at t (the A/D's sample-and-hold instant between two host samples).
template <bool Neon> inline V2<Neon> hermite(V2<Neon> xm1, V2<Neon> x0, V2<Neon> x1, V2<Neon> x2, float t) noexcept
{
    APP_FX_NOFUSE
    const V2<Neon> c1 = (x1 - xm1) * 0.5f;
    const V2<Neon> c2 = ((xm1 - x0 * 2.5f) + x1 * 2.0f) - x2 * 0.5f;
    const V2<Neon> c3 = (x2 - xm1) * 0.5f + (x0 - x1) * 1.5f;
    return ((c3 * t + c2) * t + c1) * t + x0;
}
} // namespace engine::fx::dsp::vpitch
