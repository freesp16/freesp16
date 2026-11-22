// Building blocks of the Reverb (spec 02-features/effects/reverb.md FX-820…828, FX-860…862, quirks
// FX-880…886; research docs/effects/send-reverb.md F121–F127, CC BY 4.0). The structure and every law are operation for
// operation those of the MIT research model tools/fx/send_reverb.py (ReverbPreDelay, ReverbComb, ReverbAllPass, pinned
// commit 8901c97); the constants (comb and all-pass delays, decay curve) are the app's own (engine/fx/effects/Reverb.h
// ReverbConstants, FX-821/822/827). Header-only, inline; the rings are allocated in prepare() only.
//
//   PreDelay  stereo ring of trunc(f32(f32(fs·101)/1000)) frames; write first, then read at (w + 1) − D with linear
//             interpolation; a change of D glides linearly over 1 s (FX-820/861). Reference profile: a 3 ms start-up
//             fade (integer read) after each reset. FX-882 (the one-past-end read) is fixed in both profiles: a read
//             position that rounds to the ring length wraps to 0.
//   Comb      one stereo comb: y = g·ring[trunc(w − D)] (D a double gliding linearly, g gliding linearly, both over the
//             room glide time t), the comb output is y; f = DampLPF(y) (a stepped first-order LP, 100 log steps
//             2 kHz…max, FX-824); ring_L[w] = x_L + a·f_L + b·f_R, ring_R[w] = x_R + b·f_L + a·f_R (the LR-swap
//             spread gains, 132-sample ramps, FX-826).
//   AllPass   y = ring[trunc(w − D)] − g·x, ring[w] = x + g·y, g = 0.7, fixed float delays (FX-827).
//
// Quirk bits honoured by the blocks (the Reverb sets them at each reset):
//   warmUp      FX-880: the comb and all-pass outputs stay muted until their ring has been filled once, then fade in
//               over 3 ms (the all-pass fade overshoots 1 within its last fading block, as the reference's does); the
//               pre-delay's 3 ms start fade. Sixteen: all outputs ungated from the reset.
//   staleRing   FX-881: a reset rewinds the indices and keeps the samples. Sixteen: rings read as zero (O(1) clear).
//   sharedRamp  FX-884: a comb's L/R delay ramps share one counter (so do the gain and the spread ramps): setting
//               one channel restarts the other with its old increment. Sixteen: independent counters.
//
// Scalar and NEON: the steady state of a comb (no ramp running, not fading) and of an all-pass computes its read
// indices four at a time (vcvt), fetches the taps by lane and multiplies/adds 4-wide; ramps, fades and the pre-delay are
// scalar in both builds. Both run the same float operations in the same order (never fused), so they are bit-equal on
// normal input.
#pragma once

#include "engine/fx/dsp/delay_kernels.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace engine::fx::dsp::reverb
{
using delay::f32;
using delay::Ring;
using delay::SteppedIir1;
using delay::truncS32;

inline const float kFade = f32(0.003); // 3 ms

// ------------------------------------------------------------------------------------------------ PreDelay

class PreDelay
{
public:
    std::size_t prepare(double fs)
    {
        APP_FX_NOFUSE
        fsF_ = static_cast<float>(fs);
        len_ = static_cast<int>((fsF_ * 101.0f) / 1000.0f);
        spm_ = fsF_ / 1000.0f;
        fadeInc_ = 1.0f / (fsF_ * kFade);
        std::size_t b = ring_[0].prepare(len_) + ring_[1].prepare(len_);
        init();
        return b;
    }
    void init() noexcept // ctor 0x111144
    {
        APP_FX_NOFUSE
        ms_ = f32(0.1);
        const float v = spm_ * ms_;
        cur_ = tgt_ = v;
        d_ = static_cast<double>(v);
        thr_ = static_cast<int>(v);
        inc_ = 0.0f;
        cnt_ = 0;
        st_ = 0;
        w_ = 0;
        count_ = 0;
        fade_ = 0.0f;
        ring_[0].clear();
        ring_[1].clear();
    }
    void set(float v) noexcept // 0x11131c
    {
        APP_FX_NOFUSE
        const float ms = v * 100.0f;
        ms_ = ms;
        if (! (ms >= f32(0.1)))
        {
            ms_ = f32(0.1);
            tgt_ = fsF_ * f32(0.00010000000475);
            return;
        }
        float f = f32(0.1);
        if (ms <= 100.0f)
            f = ms * delay::kMilli;
        if (100.0f < ms)
            ms_ = 100.0f;
        tgt_ = fsF_ * f;
    }
    void reset(bool staleRing, bool warmUp) noexcept // 0x1112b8
    {
        APP_FX_NOFUSE
        count_ = 0;
        fade_ = warmUp ? 0.0f : 1.0f;
        w_ = 0;
        inc_ = 0.0f;
        cnt_ = 0;
        st_ = 0;
        const float v = (ms_ * delay::kMilli) * fsF_;
        tgt_ = cur_ = v;
        thr_ = truncS32(spm_ * f32(0.1));
        d_ = static_cast<double>(v);
        if (! staleRing)
        {
            ring_[0].clear();
            ring_[1].clear();
        }
    }
    void process(float* x, float* y, int n) noexcept // 0x11138c, in place
    {
        APP_FX_NOFUSE
        const int L = len_;
        int w = w_;
        if (fade_ < 1.0f) // start-up fade: integer read
        {
            const int di = std::isfinite(d_) ? static_cast<int>(d_) : 0;
            float s12 = 0.0f;
            for (int k = 0; k < n; ++k)
            {
                ring_[0].put(w, x[k]);
                ring_[1].put(w, y[k]);
                w = w < L - 1 ? w + 1 : 0;
                if (thr_ < count_ + k + 1)
                    s12 = fadeInc_;
                float f = s12 + fade_;
                if (f > 1.0f)
                    f = 1.0f;
                fade_ = f;
                int i = w - di;
                if (i < 0)
                    i += L;
                x[k] = f * ring_[0].at(i);
                y[k] = ring_[1].at(i) * f;
            }
            count_ += n;
            w_ = w;
            return;
        }
        const float lenf = static_cast<float>(L);
        if (tgt_ != cur_)
        {
            cur_ = tgt_;
            st_ = -1;
            const int nn = truncS32(spm_ * 1000.0f);
            cnt_ = nn - 1;
            inc_ = static_cast<float>(static_cast<double>(tgt_) - d_) / static_cast<float>(nn);
        }
        for (int k = 0; k < n; ++k)
        {
            ring_[0].put(w, x[k]);
            ring_[1].put(w, y[k]);
            float wf;
            if (w < L - 1)
            {
                ++w;
                wf = static_cast<float>(w);
            }
            else
            {
                w = 0;
                wf = 0.0f;
            }
            cnt_ += st_;
            if (cnt_ < 0)
            {
                inc_ = 0.0f;
                st_ = 0;
                cnt_ = 0;
                d_ = static_cast<double>(cur_);
            }
            else
                d_ += static_cast<double>(inc_);
            float p = wf - static_cast<float>(d_);
            if (p < 0.0f)
                p = p + lenf;
            int i = static_cast<int>(p);
            float fr = p - static_cast<float>(i);
            if (fr < 0.0f)
                fr = -fr;
            if (i >= L) // FX-882 fix: the position rounded to the ring length is index 0
                i -= L;
            const int j = i < L - 1 ? i + 1 : 0;
            const float om = 1.0f - fr;
            x[k] = ring_[0].at(i) * om + fr * ring_[0].at(j);
            y[k] = ring_[1].at(i) * om + fr * ring_[1].at(j);
        }
        w_ = w;
    }
    double delaySamples() const noexcept { return d_; }

private:
    Ring ring_[2];
    float fsF_ = 44100.0f, spm_ = 44.1f, ms_ = 0.1f, cur_ = 0.0f, tgt_ = 0.0f, inc_ = 0.0f, fade_ = 0.0f,
          fadeInc_ = 0.0f;
    double d_ = 0.0;
    int len_ = 1, thr_ = 0, cnt_ = 0, st_ = 0, w_ = 0, count_ = 0;
};

// ------------------------------------------------------------------------------------------------ ramps

// A linear ramp pair (L/R or straight/swapped) with per-channel increments: the reference's shared counter (FX-884)
// or, Sixteen, one counter per channel. value = f32(value + inc) per sample (float kind) or d += inc (double kind).
template <typename V> struct Ramp2
{
    V v[2]{};
    float tgt[2]{}, inc[2]{};
    int cnt[2]{}, st[2]{};
    bool idle() const noexcept { return st[0] == 0 && st[1] == 0 && cnt[0] == 0 && cnt[1] == 0; }
    void start(int ch, int n, bool shared) noexcept
    {
        if (shared)
        {
            cnt[0] = cnt[1] = n - 1;
            st[0] = st[1] = -1;
        }
        else
        {
            cnt[ch] = n - 1;
            st[ch] = -1;
        }
    }
    void snap() noexcept
    {
        for (int c = 0; c < 2; ++c)
        {
            v[c] = static_cast<V>(tgt[c]);
            inc[c] = 0.0f;
            cnt[c] = 0;
            st[c] = 0;
        }
    }
    void tick(int c) noexcept // one sample of channel c
    {
        cnt[c] += st[c];
        if (cnt[c] < 0)
        {
            inc[c] = 0.0f;
            cnt[c] = 0;
            st[c] = 0;
            v[c] = static_cast<V>(tgt[c]);
        }
        else
            v[c] = static_cast<V>(v[c] + static_cast<V>(inc[c]));
    }
};

inline float clampLim(float hi, float v) noexcept // 0x11031c: [0, hi]
{
    if (v < 0.0f)
        return 0.0f;
    if (hi < v)
        return hi;
    return v;
}

// The integer tap positions of a fixed float delay d over n samples from write index w (ring length len):
// idx[k] = trunc(p), p = f32(f32(w_k) − d), p < 0 → f32(len + p) (comb 0x110b88, all-pass 0x1100d0). NEON computes
// four at a time and stores them (no NEON → core register moves, which stall the A9); the scalar reference is the same
// float arithmetic. A position that rounds up to len (FX-882) is wrapped by the caller's tap().
template <bool Neon> inline void tapIndices(int w, int len, float d, std::int32_t* idx, int n) noexcept
{
    APP_FX_NOFUSE
    const float lenf = static_cast<float>(len);
    int k = 0;
#if APP_FX_HAVE_NEON
    if constexpr (Neon)
    {
        const int32x4_t step = {0, 1, 2, 3};
        const int32x4_t vlen = vdupq_n_s32(len);
        const float32x4_t zero = vdupq_n_f32(0.0f), lv = vdupq_n_f32(lenf), vd = vdupq_n_f32(d);
        for (; k + 4 <= n; k += 4)
        {
            int32x4_t wi = vaddq_s32(vdupq_n_s32(w + k), step);
            wi = vbslq_s32(vcgeq_s32(wi, vlen), vsubq_s32(wi, vlen), wi);
            float32x4_t p = vsubq_f32(vcvtq_f32_s32(wi), vd);
            p = vbslq_f32(vcltq_f32(p, zero), vaddq_f32(lv, p), p);
            vst1q_s32(idx + k, vcvtq_s32_f32(p));
        }
    }
#endif
    for (; k < n; ++k)
    {
        int wk = w + k;
        if (wk >= len)
            wk -= len;
        float p = static_cast<float>(wk) - d;
        if (p < 0.0f)
            p = lenf + p;
        idx[k] = static_cast<std::int32_t>(p);
    }
}

// ------------------------------------------------------------------------------------------------ Comb

class Comb
{
public:
    std::size_t prepare(double fs, int len, float maxDampHz)
    {
        APP_FX_NOFUSE
        fs_ = fs;
        const float fsf = static_cast<float>(fs);
        spm_ = fsf / 1000.0f;
        len_ = len;
        fadeInc_ = 1.0f / (fsf * kFade);
        damp_.prepare(SteppedIir1::Kind::LowPass, fs, 100, 2000.0, static_cast<double>(maxDampHz));
        const std::size_t b = ring_[0].prepare(len) + ring_[1].prepare(len);
        init();
        return b;
    }
    void init() noexcept // ctor 0x1106a0
    {
        ring_[0].clear();
        ring_[1].clear();
        w_ = 0;
        count_ = 0;
        thresh_ = len_;
        fade_ = 0.0f;
        glide_ = 100.0f;
        ms_[0] = ms_[1] = 0.0f;
        d_ = {};
        g_ = {};
        s_ = {};
        s_.v[0] = s_.tgt[0] = 1.0f;
        damp_.init();
    }
    void setGlide(float t) noexcept { glide_ = t; }
    float glide() const noexcept { return glide_; }
    int n(float ms) const noexcept
    {
        APP_FX_NOFUSE
        return truncS32(spm_ * ms);
    }
    void setDelay(float ms, int ch, bool shared) noexcept // 0x1109d0
    {
        APP_FX_NOFUSE
        ms = clampLim(100.0f, ms);
        if (ms == ms_[ch])
            return;
        ms_[ch] = ms;
        const float t = (ms * delay::kMilli) * static_cast<float>(fs_);
        d_.tgt[ch] = t;
        const int nn = n(glide_);
        d_.start(ch, nn, shared);
        d_.inc[ch] = static_cast<float>(static_cast<double>(t) - d_.v[ch]) / static_cast<float>(nn);
    }
    void setGain(float g, int ch, bool shared) noexcept // 0x110a7c
    {
        APP_FX_NOFUSE
        g = clampLim(1.0f, g);
        if (g == g_.tgt[ch])
            return;
        g_.tgt[ch] = g;
        const int nn = n(glide_);
        g_.start(ch, nn, shared);
        g_.inc[ch] = (g - g_.v[ch]) / static_cast<float>(nn);
    }
    void setSpread(float v, int ch, bool shared) noexcept // 0x110afc
    {
        APP_FX_NOFUSE
        v = clampLim(1.0f, v);
        if (v == s_.tgt[ch])
            return;
        s_.tgt[ch] = v;
        const int nn = n(3.0f);
        s_.start(ch, nn, shared);
        s_.inc[ch] = (v - s_.v[ch]) / static_cast<float>(nn);
    }
    void setDampStep(std::uint32_t step) noexcept { damp_.setStep(step); }
    // 0x110940 + the damping container reset 0xf40f8 (by the caller in the reference)
    void reset(bool staleRing, bool warmUp) noexcept
    {
        w_ = 0;
        count_ = 0;
        fade_ = warmUp ? 0.0f : 1.0f;
        thresh_ = len_;
        d_.snap();
        g_.snap();
        s_.snap();
        damp_.resetState();
        if (! staleRing)
        {
            ring_[0].clear();
            ring_[1].clear();
        }
    }

    // 0x110b88: out = y (before damping); the rings get x + spread·damped(y).
    template <bool Neon>
    void process(const float* xl, const float* xr, float* ol, float* orr, int n) noexcept
    {
        const bool fading = fade_ < 1.0f;
        const int w0 = w_;
        if (! fading && d_.idle() && g_.idle())
            readSteady<Neon>(ol, orr, n);
        else
            readGeneric(ol, orr, n, fading);
        if (fading)
            count_ += n;
        alignas(16) float fl[delay::kMaxN], fr[delay::kMaxN];
        delay::copy<Neon>(fl, ol, n);
        delay::copy<Neon>(fr, orr, n);
        damp_.process<Neon>(fl, fr, n);
        if (s_.idle())
            writeSteady<Neon>(xl, xr, fl, fr, n, w0);
        else
            writeGeneric(xl, xr, fl, fr, n, w0);
    }

    int ringLen() const noexcept { return len_; }
    double delaySamples(int ch) const noexcept { return d_.v[ch]; }
    float gain(int ch) const noexcept { return g_.v[ch]; }
    float spread(int ch) const noexcept { return s_.v[ch]; }
    const SteppedIir1& damping() const noexcept { return damp_; }

private:
    float tap(int ch, float p) const noexcept // the integer read: trunc(p) (FX-882: a rounded-up end wraps to 0)
    {
        int i = static_cast<int>(p);
        if (i >= len_)
            i -= len_;
        return ring_[ch].at(i);
    }
    void readGeneric(float* ol, float* orr, int n, bool fading) noexcept
    {
        APP_FX_NOFUSE
        const float lenf = static_cast<float>(len_);
        int w = w0();
        for (int k = 0; k < n; ++k)
        {
            d_.tick(0);
            d_.tick(1);
            const float wf = static_cast<float>(w);
            float pl = wf - static_cast<float>(d_.v[0]);
            if (pl < 0.0f)
                pl = lenf + pl;
            float pr = wf - static_cast<float>(d_.v[1]);
            if (pr < 0.0f)
                pr = lenf + pr;
            const float yl0 = tap(0, pl), yr0 = tap(1, pr);
            if (fading)
            {
                if (thresh_ < k + count_)
                    fade_ = fade_ + fadeInc_;
                if (fade_ > 1.0f)
                    fade_ = 1.0f;
            }
            g_.tick(0);
            g_.tick(1);
            if (fading)
            {
                ol[k] = (fade_ * yl0) * g_.v[0];
                orr[k] = (fade_ * yr0) * g_.v[1];
            }
            else
            {
                ol[k] = g_.v[0] * yl0;
                orr[k] = g_.v[1] * yr0;
            }
            w = w < len_ - 1 ? w + 1 : 0;
        }
    }
    template <bool Neon> void readSteady(float* ol, float* orr, int n) noexcept
    {
        alignas(16) std::int32_t il[delay::kMaxN], ir[delay::kMaxN];
        tapIndices<Neon>(w_, len_, static_cast<float>(d_.v[0]), il, n);
        tapIndices<Neon>(w_, len_, static_cast<float>(d_.v[1]), ir, n);
        for (int k = 0; k < n; ++k)
        {
            ol[k] = tapI(0, il[k]);
            orr[k] = tapI(1, ir[k]);
        }
        delay::mul<Neon>(ol, g_.v[0], n); // y = g · ring
        delay::mul<Neon>(orr, g_.v[1], n);
    }
    float tapI(int ch, int i) const noexcept
    {
        if (i >= len_)
            i -= len_;
        return ring_[ch].at(i);
    }
    void writeGeneric(const float* xl, const float* xr, const float* fl, const float* fr, int n, int w) noexcept
    {
        APP_FX_NOFUSE
        for (int k = 0; k < n; ++k)
        {
            s_.tick(0);
            s_.tick(1);
            const float a = s_.v[0], b = s_.v[1];
            ring_[0].put(w, (xl[k] + a * fl[k]) + b * fr[k]);
            ring_[1].put(w, (xr[k] + b * fl[k]) + a * fr[k]);
            w = w < len_ - 1 ? w + 1 : 0;
        }
        ring_[0].setW(w);
        ring_[1].setW(w);
        w_ = w;
    }
    template <bool Neon>
    void writeSteady(const float* xl, const float* xr, const float* fl, const float* fr, int n, int w) noexcept
    {
        APP_FX_NOFUSE
        const float a = s_.v[0], b = s_.v[1];
        alignas(16) float tl[delay::kMaxN], tr[delay::kMaxN];
        int k = 0;
#if APP_FX_HAVE_NEON
        if constexpr (Neon)
        {
            for (; k + 4 <= n; k += 4)
            {
                const float32x4_t vfl = vld1q_f32(fl + k), vfr = vld1q_f32(fr + k);
                vst1q_f32(tl + k, vaddq_f32(vaddq_f32(vld1q_f32(xl + k), vmulq_n_f32(vfl, a)), vmulq_n_f32(vfr, b)));
                vst1q_f32(tr + k, vaddq_f32(vaddq_f32(vld1q_f32(xr + k), vmulq_n_f32(vfl, b)), vmulq_n_f32(vfr, a)));
            }
        }
#endif
        for (; k < n; ++k)
        {
            tl[k] = (xl[k] + a * fl[k]) + b * fr[k];
            tr[k] = (xr[k] + b * fl[k]) + a * fr[k];
        }
        ring_[0].setW(w);
        ring_[1].setW(w);
        ring_[0].write(tl, n);
        ring_[1].write(tr, n);
        w_ = ring_[0].w();
    }
    int w0() const noexcept { return w_; }

    double fs_ = 44100.0;
    float spm_ = 44.1f, fadeInc_ = 0.0f, fade_ = 0.0f, glide_ = 100.0f;
    float ms_[2]{};
    int len_ = 1, w_ = 0, count_ = 0, thresh_ = 1;
    Ring ring_[2];
    Ramp2<double> d_;
    Ramp2<float> g_, s_;
    SteppedIir1 damp_;
};

// ------------------------------------------------------------------------------------------------ AllPass

class AllPass
{
public:
    std::size_t prepare(double fs)
    {
        APP_FX_NOFUSE
        fs_ = fs;
        const float fsf = static_cast<float>(fs);
        len_ = static_cast<int>((static_cast<long long>(fs) * 101) / 1000);
        fadeInc_ = 1.0f / (fsf * kFade);
        const std::size_t b = ring_[0].prepare(len_) + ring_[1].prepare(len_);
        init();
        return b;
    }
    void init() noexcept
    {
        ring_[0].clear();
        ring_[1].clear();
        ms_[0] = ms_[1] = dsamp_[0] = dsamp_[1] = g_[0] = g_[1] = ng_[0] = ng_[1] = 0.0f;
        w_ = 0;
        count_ = 0;
        thresh_ = len_;
        fade_ = 0.0f;
    }
    void setDelay(float ms, int ch) noexcept // 0x11003c
    {
        APP_FX_NOFUSE
        ms_[ch] = ms;
        dsamp_[ch] = (ms * delay::kMilli) * static_cast<float>(fs_);
    }
    void setGain(float g, int ch) noexcept // 0x110090
    {
        g_[ch] = g;
        ng_[ch] = -g;
    }
    void reset(bool staleRing, bool warmUp) noexcept // 0x11001c
    {
        w_ = 0;
        fade_ = warmUp ? 0.0f : 1.0f;
        count_ = 0;
        thresh_ = len_;
        if (! staleRing)
        {
            ring_[0].clear();
            ring_[1].clear();
        }
    }
    template <bool Neon> void process(float* x, float* y, int n) noexcept // 0x1100d0, in place
    {
        APP_FX_NOFUSE
        const bool fading = fade_ < 1.0f;
        const float lenf = static_cast<float>(len_);
        const float minD = dsamp_[0] < dsamp_[1] ? dsamp_[0] : dsamp_[1];
        if (! fading && minD > static_cast<float>(n + 1))
        {
            steady<Neon>(x, y, n);
            return;
        }
        float s11 = 0.0f;
        int w = w_;
        for (int k = 0; k < n; ++k)
        {
            const float wf = static_cast<float>(w);
            float pl = wf - dsamp_[0];
            if (pl < 0.0f)
                pl = lenf + pl;
            float pr = wf - dsamp_[1];
            if (pr < 0.0f)
                pr = lenf + pr;
            const float yl0 = tapI(0, static_cast<int>(pl)), yr0 = tapI(1, static_cast<int>(pr));
            const float xl = x[k], xr = y[k];
            float yl, yr;
            if (fading)
            {
                if (thresh_ < k + count_)
                    s11 = fadeInc_;
                fade_ = fade_ + s11;
                yl = fade_ * yl0 + ng_[0] * xl;
                yr = fade_ * yr0 + ng_[1] * xr;
            }
            else
            {
                yl = yl0 + ng_[0] * xl;
                yr = yr0 + ng_[1] * xr;
            }
            ring_[0].put(w, xl + g_[0] * yl);
            ring_[1].put(w, xr + g_[1] * yr);
            x[k] = yl;
            y[k] = yr;
            w = w < len_ - 1 ? w + 1 : 0;
        }
        if (fading)
            count_ += n;
        w_ = w;
        ring_[0].setW(w);
        ring_[1].setW(w);
    }
    int ringLen() const noexcept { return len_; }

private:
    float tapI(int ch, int i) const noexcept
    {
        if (i >= len_)
            i -= len_;
        return ring_[ch].at(i);
    }
    // Not fading and D > n + 1: every read is older than this block's writes, so reads first, then the writes.
    template <bool Neon> void steady(float* x, float* y, int n) noexcept
    {
        APP_FX_NOFUSE
        alignas(16) std::int32_t il[delay::kMaxN], ir[delay::kMaxN];
        alignas(16) float tl[delay::kMaxN], tr[delay::kMaxN];
        tapIndices<Neon>(w_, len_, dsamp_[0], il, n);
        tapIndices<Neon>(w_, len_, dsamp_[1], ir, n);
        for (int k = 0; k < n; ++k)
        {
            tl[k] = tapI(0, il[k]);
            tr[k] = tapI(1, ir[k]);
        }
        int k = 0;
#if APP_FX_HAVE_NEON
        if constexpr (Neon)
        {
            for (; k + 4 <= n; k += 4)
            {
                const float32x4_t xl = vld1q_f32(x + k), xr = vld1q_f32(y + k);
                const float32x4_t yl = vaddq_f32(vld1q_f32(tl + k), vmulq_n_f32(xl, ng_[0]));
                const float32x4_t yr = vaddq_f32(vld1q_f32(tr + k), vmulq_n_f32(xr, ng_[1]));
                vst1q_f32(tl + k, vaddq_f32(xl, vmulq_n_f32(yl, g_[0]))); // the ring values
                vst1q_f32(tr + k, vaddq_f32(xr, vmulq_n_f32(yr, g_[1])));
                vst1q_f32(x + k, yl);
                vst1q_f32(y + k, yr);
            }
        }
#endif
        for (; k < n; ++k)
        {
            const float yl = tl[k] + ng_[0] * x[k];
            const float yr = tr[k] + ng_[1] * y[k];
            tl[k] = x[k] + g_[0] * yl;
            tr[k] = y[k] + g_[1] * yr;
            x[k] = yl;
            y[k] = yr;
        }
        ring_[0].setW(w_);
        ring_[1].setW(w_);
        ring_[0].write(tl, n);
        ring_[1].write(tr, n);
        w_ = ring_[0].w();
    }

    double fs_ = 44100.0;
    float ms_[2]{}, dsamp_[2]{}, g_[2]{}, ng_[2]{};
    int len_ = 1, w_ = 0, count_ = 0, thresh_ = 1;
    float fade_ = 0.0f, fadeInc_ = 0.0f;
    Ring ring_[2];
};
} // namespace engine::fx::dsp::reverb
