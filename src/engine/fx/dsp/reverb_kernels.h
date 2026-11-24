// Building blocks of the Reverb (engine/fx/effects/Reverb.h): an 8-line feedback delay network with input diffusion.
// Header-only, inline; the rings are allocated in prepare() only, every block function takes n <= kMaxN (64) samples.
//
//   Ring       one mono ring of exact length (no power-of-two rounding: one Reverb runs in each of the 34 FX slots).
//              Reads address an age (the sample written `age` writes ago, 1 = newest); a block read at integer age
//              copies at most two contiguous segments, a gliding read interpolates linearly. clear() is O(1): a fill
//              counter makes every sample written before it read as 0 (kClearState / second STOP never memset).
//   AllPass    a Schroeder all-pass of fixed integer delay M >= n + 1: z = v[t − M], v[t] = x + g·z, y = z − g·v[t].
//              Every read of a block is older than the block's writes, so the block runs as vector operations (no
//              per-sample recursion): read z, v = x + g·z, y = z − g·v, write v.
//   Fdn8       the late reverb. Eight lines of different lengths (mutually prime in samples); per sample
//                s_i = a_i·s_i + b_i·y_i                  one-pole absorbent filter per line (decay and damping)
//                u   = H8 · s                             unnormalised Walsh–Hadamard (±1), 1/√8 folded into b_i
//                v_i = u_i + c_i·in                       lines 1–3 take the left input, 5–7 the right
//              and the outputs are v_0 ± v_4 (lines 0 and 4 take no input: v_0 = Σ s, v_4 = Σ s_lo − Σ s_hi, so
//              left = 2·Σ s_lo, right = 2·Σ s_hi — the left input's first arrivals reach the left output only, the
//              matrix decorrelates the tail). Every line is at least n + 2 samples long, so all eight reads of a block
//              precede its writes: read 8 blocks, run the matrix per sample, write 8 blocks.
//              NEON: lines 0–3 and 4–7 are two float32x4 lanes sets; 4 samples of the 8 line blocks are transposed
//              (vtrn/vcombine), the recursion and the 8-point Hadamard (vrev64 + vext butterflies) run per sample on
//              the two vectors, and the results are transposed back. The scalar path runs the same float operations
//              in the same order (never fused, APP_FX_NOFUSE), so both are bit-equal on normal input.
//
// The absorbent filter of a line of D samples, for a decay time T (s) and a damping frequency fd: the DC gain is
// g = 10^(−3·D/(fs·T)) (−60 dB after T), and the loss in dB per pass at fd is kDampRatio times the DC loss
// (T60(fd) = T / kDampRatio), solved exactly for a one-pole H(z) = b/(1 − a·z⁻¹): with q = g^(2·(kDampRatio − 1))
// and c = cos(2π·fd/fs), B = (1 − q·c)/(1 − q), a = B − √(B² − 1), b = g·(1 − a). Every line decays at the same rate
// per second at every frequency (Jot's absorbent-filter condition, the one-pole approximation), so the tail has no
// ringing modes of the long lines. The pole is capped at a plain one-pole low-pass at fd (absorbent() below).
#pragma once

#include "engine/fx/dsp/delay_kernels.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

#if APP_FX_HAVE_NEON
#include <arm_neon.h>
#endif

namespace engine::fx::dsp::reverb
{
using delay::kMaxN;

// ------------------------------------------------------------------------------------------------ Ring

class Ring
{
public:
    std::size_t prepare(int len) // message thread
    {
        len_ = len < 2 ? 2 : len;
        buf_ = std::make_unique<float[]>(static_cast<std::size_t>(len_)); // rt-ok: prepare(), message thread
        w_ = 0;
        fill_ = 0;
        return sizeof(float) * static_cast<std::size_t>(len_);
    }
    void clear() noexcept { fill_ = 0; } // O(1)
    int length() const noexcept { return len_; }
    int maxAge() const noexcept { return len_ - 1; }

    // out[k] = the sample of age `age − k` (k = 0…n−1), measured before this block's writes; n <= age <= maxAge().
    void readInt(int age, float* out, int n) const noexcept
    {
        if (fill_ < age) // partly before the last clear(): those samples read as 0
        {
            for (int k = 0; k < n; ++k)
            {
                const int a = age - k;
                out[k] = a <= fill_ ? buf_[idx(a)] : 0.0f;
            }
            return;
        }
        int i = w_ - age;
        if (i < 0)
            i += len_;
        const int first = len_ - i < n ? len_ - i : n;
        std::memcpy(out, buf_.get() + i, sizeof(float) * static_cast<std::size_t>(first));
        if (first < n)
            std::memcpy(out + first, buf_.get(), sizeof(float) * static_cast<std::size_t>(n - first));
    }
    // Linear-interpolated read of a gliding age: sample k at age a0 + k·da (a0, a0 + (n−1)·da >= n + 1, so both
    // neighbours precede the block's writes; <= maxAge() − 1).
    // The read position of sample k walks linearly: p_k = (w − a0) + k·(1 − da) (w = the write index at the block
    // start), wrapped into [0, len); j = trunc(p_k) is the older neighbour, j + 1 the newer, weight p_k − j.
    template <bool Neon> void readLerp(float a0, float da, float* out, int n) const noexcept
    {
        APP_FX_NOFUSE
        const float aMax = a0 > a0 + static_cast<float>(n - 1) * da ? a0 : a0 + static_cast<float>(n - 1) * da;
        const float lenf = static_cast<float>(len_);
        const float p0 = static_cast<float>(w_) - a0, step = 1.0f - da;
        const float* b = buf_.get();
        if (static_cast<float>(fill_) < aMax + 1.0f) // partly before the last clear(): those samples read as 0
        {
            for (int k = 0; k < n; ++k)
            {
                const float a = (a0 + static_cast<float>(k) * da) - static_cast<float>(k); // age from the block start
                const int ai = static_cast<int>(a);
                const float fr = a - static_cast<float>(ai);
                const float x0 = ai <= fill_ ? b[idx(ai)] : 0.0f, x1 = ai + 1 <= fill_ ? b[idx(ai + 1)] : 0.0f;
                out[k] = x0 + (x1 - x0) * fr;
            }
            return;
        }
        int k = 0;
#if APP_FX_HAVE_NEON
        if constexpr (Neon)
        {
            const float32x4_t vp0 = vdupq_n_f32(p0), vstep = vdupq_n_f32(step), vlen = vdupq_n_f32(lenf),
                              zero = vdupq_n_f32(0.0f);
            const int32x4_t ilen = vdupq_n_s32(len_), one = vdupq_n_s32(1), izero = vdupq_n_s32(0);
            float32x4_t kv = {0.0f, 1.0f, 2.0f, 3.0f};
            const float32x4_t four = vdupq_n_f32(4.0f);
            alignas(16) std::int32_t j0[4], j1[4];
            for (; k + 4 <= n; k += 4)
            {
                float32x4_t p = vaddq_f32(vp0, vmulq_f32(kv, vstep));
                p = vbslq_f32(vcltq_f32(p, zero), vaddq_f32(p, vlen), p);
                const int32x4_t j = vcvtq_s32_f32(p);
                const float32x4_t fr = vsubq_f32(p, vcvtq_f32_s32(j));
                int32x4_t jn = vaddq_s32(j, one);
                jn = vbslq_s32(vceqq_s32(jn, ilen), izero, jn);
                vst1q_s32(j0, j);
                vst1q_s32(j1, jn);
                float32x4_t x0 = vdupq_n_f32(0.0f), x1 = vdupq_n_f32(0.0f);
                x0 = vld1q_lane_f32(b + j0[0], x0, 0);
                x0 = vld1q_lane_f32(b + j0[1], x0, 1);
                x0 = vld1q_lane_f32(b + j0[2], x0, 2);
                x0 = vld1q_lane_f32(b + j0[3], x0, 3);
                x1 = vld1q_lane_f32(b + j1[0], x1, 0);
                x1 = vld1q_lane_f32(b + j1[1], x1, 1);
                x1 = vld1q_lane_f32(b + j1[2], x1, 2);
                x1 = vld1q_lane_f32(b + j1[3], x1, 3);
                vst1q_f32(out + k, vaddq_f32(x0, vmulq_f32(vsubq_f32(x1, x0), fr)));
                kv = vaddq_f32(kv, four);
            }
        }
#endif
        for (; k < n; ++k)
        {
            float p = p0 + static_cast<float>(k) * step;
            if (p < 0.0f)
                p = p + lenf;
            const int j = static_cast<int>(p);
            const float fr = p - static_cast<float>(j);
            const int jn = j + 1 == len_ ? 0 : j + 1;
            out[k] = b[j] + (b[jn] - b[j]) * fr;
        }
    }
    void write(const float* in, int n) noexcept
    {
        const int first = len_ - w_ < n ? len_ - w_ : n;
        std::memcpy(buf_.get() + w_, in, sizeof(float) * static_cast<std::size_t>(first));
        if (first < n)
            std::memcpy(buf_.get(), in + first, sizeof(float) * static_cast<std::size_t>(n - first));
        w_ += n;
        if (w_ >= len_)
            w_ -= len_;
        fill_ = fill_ + n < len_ ? fill_ + n : len_;
    }

private:
    // physical index of `age` measured from the write index at the block start (age may reach age + n − 1 via k)
    int idx(int age) const noexcept
    {
        int i = w_ - age;
        while (i < 0)
            i += len_;
        return i;
    }
    std::unique_ptr<float[]> buf_;
    int len_ = 2, w_ = 0, fill_ = 0;
};

// ------------------------------------------------------------------------------------------------ AllPass

class AllPass
{
public:
    std::size_t prepare(int delaySamples, float g) // delaySamples >= kMaxN + 1
    {
        m_ = delaySamples < kMaxN + 1 ? kMaxN + 1 : delaySamples;
        g_ = g;
        return ring_.prepare(m_ + 1);
    }
    void clear() noexcept { ring_.clear(); }
    int delay() const noexcept { return m_; }
    template <bool Neon> void process(float* x, int n) noexcept // in place
    {
        APP_FX_NOFUSE
        alignas(16) float z[kMaxN], v[kMaxN];
        ring_.readInt(m_, z, n);
        const float g = g_;
        int k = 0;
#if APP_FX_HAVE_NEON
        if constexpr (Neon)
        {
            for (; k + 4 <= n; k += 4)
            {
                const float32x4_t zz = vld1q_f32(z + k);
                const float32x4_t vv = vaddq_f32(vld1q_f32(x + k), vmulq_n_f32(zz, g));
                vst1q_f32(v + k, vv);
                vst1q_f32(x + k, vsubq_f32(zz, vmulq_n_f32(vv, g)));
            }
        }
#endif
        for (; k < n; ++k)
        {
            const float vv = x[k] + z[k] * g;
            v[k] = vv;
            x[k] = z[k] - vv * g;
        }
        ring_.write(v, n);
    }

private:
    Ring ring_;
    int m_ = kMaxN + 1;
    float g_ = 0.0f;
};

// ------------------------------------------------------------------------------------------------ Fdn8

class Fdn8
{
public:
    static constexpr int kLines = 8;

    // maxDelay[i]: the longest delay line i may glide to (samples, >= kMaxN + 2).
    std::size_t prepare(const float (&maxDelay)[kLines])
    {
        std::size_t b = 0;
        for (int i = 0; i < kLines; ++i)
            b += ring_[i].prepare(static_cast<int>(maxDelay[i]) + 4);
        clear();
        return b;
    }
    void clear() noexcept
    {
        for (auto& r : ring_)
            r.clear();
        for (auto& s : st_)
            s = 0.0f;
    }
    // Input gains: lines 0–3 take the left input, lines 4–7 the right (gain[i] × its side's sample).
    void setInputGains(const float (&gain)[kLines]) noexcept
    {
        for (int i = 0; i < kLines; ++i)
            c_[i] = gain[i];
    }
    // The line filters (b already includes the 1/√8 of the matrix).
    void setFilter(int i, float a, float b) noexcept
    {
        a_[i] = a;
        b_[i] = b;
    }
    float filterA(int i) const noexcept { return a_[i]; }
    float filterB(int i) const noexcept { return b_[i]; }
    int maxAge(int i) const noexcept { return ring_[i].maxAge(); }

    // One block. delay0/delay1: each line's delay (samples) at the block start and at its end (equal integers: the
    // plain copy path). inL/inR: the network input; outL/outR: its output (may alias neither input).
    template <bool Neon>
    void process(const float* inL, const float* inR, float* outL, float* outR, const float (&delay0)[kLines],
                 const float (&delay1)[kLines], int n) noexcept
    {
        APP_FX_NOFUSE
        alignas(16) float y[kLines][kMaxN];
        alignas(16) float v[kLines][kMaxN];
        for (int i = 0; i < kLines; ++i)
        {
            const float d0 = delay0[i], d1 = delay1[i];
            const int di = static_cast<int>(d0);
            if (d0 == d1 && static_cast<float>(di) == d0)
                ring_[i].readInt(di, y[i], n);
            else
                ring_[i].template readLerp<Neon>(d0, (d1 - d0) / static_cast<float>(n), y[i], n);
        }
        int k = 0;
#if APP_FX_HAVE_NEON
        if constexpr (Neon)
            k = matrixNeon(y, v, inL, inR, n);
#endif
        matrixScalar(y, v, inL, inR, k, n);
        for (int i = 0; i < kLines; ++i)
            ring_[i].write(v[i], n);
        // outputs: left = v0 + v4 = 2·Σ s_lo, right = v0 − v4 = 2·Σ s_hi (lines 0 and 4 take no input)
        int j = 0;
#if APP_FX_HAVE_NEON
        if constexpr (Neon)
            for (; j + 4 <= n; j += 4)
            {
                const float32x4_t a = vld1q_f32(v[0] + j), c = vld1q_f32(v[4] + j);
                vst1q_f32(outL + j, vaddq_f32(a, c));
                vst1q_f32(outR + j, vsubq_f32(a, c));
            }
#endif
        for (; j < n; ++j)
        {
            outL[j] = v[0][j] + v[4][j];
            outR[j] = v[0][j] - v[4][j];
        }
    }

private:
    void matrixScalar(const float (&y)[kLines][kMaxN], float (&v)[kLines][kMaxN], const float* inL, const float* inR,
                      int k0, int n) noexcept
    {
        APP_FX_NOFUSE
        float lo[4], hi[4];
        for (int i = 0; i < 4; ++i)
        {
            lo[i] = st_[i];
            hi[i] = st_[i + 4];
        }
        for (int k = k0; k < n; ++k)
        {
            float p[4], m[4], uLo[4], uHi[4];
            for (int i = 0; i < 4; ++i)
            {
                lo[i] = lo[i] * a_[i] + y[i][k] * b_[i];
                hi[i] = hi[i] * a_[i + 4] + y[i + 4][k] * b_[i + 4];
                p[i] = lo[i] + hi[i];
                m[i] = lo[i] - hi[i];
            }
            h4(p, uLo);
            h4(m, uHi);
            const float xl = inL[k], xr = inR[k];
            for (int i = 0; i < 4; ++i)
            {
                v[i][k] = uLo[i] + c_[i] * xl;
                v[i + 4][k] = uHi[i] + c_[i + 4] * xr;
            }
        }
        for (int i = 0; i < 4; ++i)
        {
            st_[i] = lo[i];
            st_[i + 4] = hi[i];
        }
    }
    // The 4-point Walsh–Hadamard butterflies in the NEON lane order: t = x·(+,−,+,−) + swap-pairs(x),
    // u = t·(+,+,−,−) + swap-halves(t) (x·(−1) + y is exactly y − x).
    static void h4(const float* x, float* u) noexcept
    {
        APP_FX_NOFUSE
        const float t0 = x[0] * 1.0f + x[1], t1 = x[1] * -1.0f + x[0];
        const float t2 = x[2] * 1.0f + x[3], t3 = x[3] * -1.0f + x[2];
        u[0] = t0 * 1.0f + t2;
        u[1] = t1 * 1.0f + t3;
        u[2] = t2 * -1.0f + t0;
        u[3] = t3 * -1.0f + t1;
    }

#if APP_FX_HAVE_NEON
    static inline void transpose(float32x4_t& r0, float32x4_t& r1, float32x4_t& r2, float32x4_t& r3) noexcept
    {
        const float32x4x2_t a = vtrnq_f32(r0, r1), b = vtrnq_f32(r2, r3);
        r0 = vcombine_f32(vget_low_f32(a.val[0]), vget_low_f32(b.val[0]));
        r1 = vcombine_f32(vget_low_f32(a.val[1]), vget_low_f32(b.val[1]));
        r2 = vcombine_f32(vget_high_f32(a.val[0]), vget_high_f32(b.val[0]));
        r3 = vcombine_f32(vget_high_f32(a.val[1]), vget_high_f32(b.val[1]));
    }
    static inline float32x4_t h4(float32x4_t x, float32x4_t sA, float32x4_t sB) noexcept
    {
        const float32x4_t t = vaddq_f32(vmulq_f32(x, sA), vrev64q_f32(x));
        return vaddq_f32(vmulq_f32(t, sB), vextq_f32(t, t, 2));
    }
    // Four samples at a time; returns the samples done (the scalar path finishes the tail and keeps the state).
    int matrixNeon(const float (&y)[kLines][kMaxN], float (&v)[kLines][kMaxN], const float* inL, const float* inR,
                   int n) noexcept
    {
        const float32x4_t sA = {1.0f, -1.0f, 1.0f, -1.0f}, sB = {1.0f, 1.0f, -1.0f, -1.0f};
        const float32x4_t aLo = vld1q_f32(a_), aHi = vld1q_f32(a_ + 4), bLo = vld1q_f32(b_), bHi = vld1q_f32(b_ + 4);
        const float32x4_t cLo = vld1q_f32(c_), cHi = vld1q_f32(c_ + 4);
        float32x4_t sLo = vld1q_f32(st_), sHi = vld1q_f32(st_ + 4);
        int k = 0;
        for (; k + 4 <= n; k += 4)
        {
            float32x4_t lo[4] = {vld1q_f32(y[0] + k), vld1q_f32(y[1] + k), vld1q_f32(y[2] + k), vld1q_f32(y[3] + k)};
            float32x4_t hi[4] = {vld1q_f32(y[4] + k), vld1q_f32(y[5] + k), vld1q_f32(y[6] + k), vld1q_f32(y[7] + k)};
            transpose(lo[0], lo[1], lo[2], lo[3]); // lo[j] = lines 0–3 at sample k + j
            transpose(hi[0], hi[1], hi[2], hi[3]);
            for (int j = 0; j < 4; ++j)
            {
                sLo = vaddq_f32(vmulq_f32(sLo, aLo), vmulq_f32(lo[j], bLo));
                sHi = vaddq_f32(vmulq_f32(sHi, aHi), vmulq_f32(hi[j], bHi));
                const float32x4_t uLo = h4(vaddq_f32(sLo, sHi), sA, sB);
                const float32x4_t uHi = h4(vsubq_f32(sLo, sHi), sA, sB);
                lo[j] = vaddq_f32(uLo, vmulq_n_f32(cLo, inL[k + j]));
                hi[j] = vaddq_f32(uHi, vmulq_n_f32(cHi, inR[k + j]));
            }
            transpose(lo[0], lo[1], lo[2], lo[3]); // back to lines × samples
            transpose(hi[0], hi[1], hi[2], hi[3]);
            for (int i = 0; i < 4; ++i)
            {
                vst1q_f32(v[i] + k, lo[i]);
                vst1q_f32(v[i + 4] + k, hi[i]);
            }
        }
        vst1q_f32(st_, sLo);
        vst1q_f32(st_ + 4, sHi);
        return k;
    }
#endif

    Ring ring_[kLines];
    alignas(16) float st_[kLines]{};
    alignas(16) float a_[kLines]{};
    alignas(16) float b_[kLines]{};
    alignas(16) float c_[kLines]{};
};

// ------------------------------------------------------------------------------------------------ helpers

// The one-pole absorbent filter of a line (see the header): DC gain g, T60(fd) = T60 / ratio — but never darker per
// pass than a plain one-pole low-pass at fd (−3 dB at fd): with a short T60 the ratio alone would put the pole
// near 1 and darken everything far below fd.
inline void absorbent(double delaySamples, double fs, double t60, double fd, double ratio, float& a, float& b) noexcept
{
    const double g = std::pow(10.0, -3.0 * delaySamples / (fs * t60));
    const double q = std::pow(g, 2.0 * (ratio - 1.0));
    const double c = std::cos(2.0 * delay::kPi * fd / fs);
    double pole = 0.0;
    if (q < 1.0)
    {
        const double B = (1.0 - q * c) / (1.0 - q);
        pole = B - std::sqrt(B * B - 1.0);
    }
    const double cut = (2.0 - c) - std::sqrt((2.0 - c) * (2.0 - c) - 1.0); // |H(fd)|/|H(0)| = 1/√2
    pole = pole < cut ? pole : cut;
    a = static_cast<float>(pole);
    b = static_cast<float>(g * (1.0 - pole));
}

// A slow sine (the line modulation): a phasor rotated once per block, renormalised so it never drifts.
struct BlockSine
{
    float c = 1.0f, s = 0.0f, rc = 1.0f, rs = 0.0f;
    void setRate(double hz, double fs, int n) noexcept
    {
        const double w = 2.0 * delay::kPi * hz * static_cast<double>(n) / fs;
        rc = static_cast<float>(std::cos(w));
        rs = static_cast<float>(std::sin(w));
    }
    void setPhase(double turns) noexcept
    {
        c = static_cast<float>(std::cos(2.0 * delay::kPi * turns));
        s = static_cast<float>(std::sin(2.0 * delay::kPi * turns));
    }
    float advance() noexcept // the value at the end of the block
    {
        APP_FX_NOFUSE
        const float nc = c * rc - s * rs, ns = s * rc + c * rs;
        const float k = 1.5f - 0.5f * (nc * nc + ns * ns);
        c = nc * k;
        s = ns * k;
        return s;
    }
};
} // namespace engine::fx::dsp::reverb
