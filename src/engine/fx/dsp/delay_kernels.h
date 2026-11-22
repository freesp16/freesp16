// Building blocks of the Insert Delay, the Send/Master Delay and the Reverb (spec 02-features/effects/
// {insert-delay,send-delay,reverb}.md; research docs/effects/{insert-delay,send-delay,send-reverb,numerics}.md, CC BY
// 4.0). Operation for operation equal to the shared blocks of the MIT research models tools/fx/insert_delay.py
// (RingBuffer, DelayModuleStereo, HiCut) and send_delay.py (DelayModuleMono, SteppedFilter, smooth_mul), pinned commit
// 8901c97, with lofi.py's SmoothFloat / SignalGate (the kit's dsp::SmoothFloat). Header-only, inline, no allocation
// outside prepare(); every block function takes n <= 64 samples.
//
//   Param       the reference LinearParameter ("last" vs "current", |last − cur| > FLT_MIN) and ToggleParameter;
//               untrap() makes the next changed() true whatever the value (Sixteen: FX-056 / FX-682 / FX-782).
//   Ring        one channel of the reference RingBuffer with its physical layout (length, write index, a separate
//               read pointer): the Reference profile's reset rewinds the indices and keeps the samples (FX-681), so a
//               later read can return stale audio exactly as the reference does. clear() is O(1): a high-water mark
//               makes every sample written before it read as 0 (FX-681/781/881 fix without a memset).
//   Gate        the 0/1 SignalGate (147 samples, built closed) of a delay module.
//   Module      DelayModuleStereo / DelayModule: integer delay D, time change through the gate (fade out, jump at the
//               end of the fade, fade in, FX-622/724), the warm-up gate (FX-680/780, Reference profile only).
//   SteppedIir1 the stepped first-order bilinear low-/high-pass (HiCut / SteppedFilter, FX-621/722/824): the cutoff
//               glides one step (×m, accumulated in double) per 1/3 ms, coefficients from tan() in double stored as
//               float32, y = f32(f32(f32(b0·x) + f32(b1·x1)) − f32(a1·y1)); L/R in two NEON lanes. The coefficient
//               routine runs only when the clamped cutoff changes (the result is bit-identical to the reference's
//               every-block recomputation, and saves a tan() per block on the A9).
//
// Scalar and NEON: every kernel templated on <bool Neon> runs the same float operations in the same order (products
// rounded before the sums, never fused: APP_FX_NOFUSE), so both are bit-equal on normal inputs. NEON flushes
// denormals; so does the app on its audio thread (FTZ, FX-055).
#pragma once

#include "engine/fx/dsp/Numerics.h"
#include "engine/fx/dsp/Ramps.h"
#include "engine/fx/dsp/Vec.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>

#if APP_FX_HAVE_NEON
#include <arm_neon.h>
#endif

namespace engine::fx::dsp::delay
{
constexpr int kMaxN = 64;
constexpr float kFltMin = 1.17549435e-38f;
constexpr double kPi = 3.141592653589793;
constexpr double kInv2Pi = 0.15915494309189535;
constexpr double kTwoPi = 6.283185307179586;

// The models' float32 constants: F(x) of a double literal (a float literal could round differently, F(decimal)).
inline float f32(double x) noexcept
{
    return static_cast<float>(x);
}
inline const float kMilli = f32(0.001);         // 0x3a83126f
inline const float kRampMs = f32(3.3333333);    // gate / SmoothFloat ramp time
inline const float kPeriodMs = f32(0.33333331); // stepped filter: one step per 1/3 ms
inline const float kFourThirds = f32(1.3333334);

// rint_idx(lo, hi, v): i = round-half-even(f32(lo + f32(v·(hi − 1 − lo)))) (chorus.py, the BasicParamSpec index)
inline int rintIdx(int lo, int hi, float v) noexcept
{
    APP_FX_NOFUSE
    const float x = static_cast<float>(lo) + v * static_cast<float>(hi - 1 - lo);
    return static_cast<int>(std::nearbyint(static_cast<double>(x)));
}
// vcvt.s32.f32: toward zero, saturating, NaN → 0
inline std::int32_t truncS32(float x) noexcept
{
    if (std::isnan(x))
        return 0;
    if (x >= 2147483647.0f)
        return 2147483647;
    if (x <= -2147483648.0f)
        return std::numeric_limits<std::int32_t>::min();
    return static_cast<std::int32_t>(x);
}
// vcvt.u32.f32: toward zero, saturating at 0 and 2^32 − 1, NaN → 0
inline std::uint32_t truncU32(float x) noexcept
{
    if (std::isnan(x) || x <= 0.0f)
        return 0u;
    if (x >= 4294967295.0f)
        return 4294967295u;
    return static_cast<std::uint32_t>(x);
}
// N_ramp = trunc(f32(f32(3.3333333 · 0.001) · fs)): 147 @ 44.1 kHz, 160 @ 48 kHz, 320 @ 96 kHz
inline int rampSamples(double fs) noexcept
{
    APP_FX_NOFUSE
    return static_cast<int>((kRampMs * kMilli) * static_cast<float>(fs));
}

// ------------------------------------------------------------------------------------------------ Param

struct Param
{
    float last = 0.0f, cur = 0.0f;
    bool untrapped = false;
    void init(float v) noexcept
    {
        last = cur = v;
        untrapped = false;
    }
    void set(float v) noexcept { cur = v; }
    void untrap() noexcept { untrapped = true; }
    bool changed() noexcept // 0xf0ffc
    {
        APP_FX_NOFUSE
        if (untrapped)
        {
            untrapped = false;
            last = cur;
            return true;
        }
        const float d = last - cur;
        if (d <= kFltMin && -kFltMin <= d)
            return false;
        last = cur;
        return true;
    }
    float get() const noexcept { return cur; }
};

struct Toggle // ToggleParameter: only v >= 1 or v <= 0 is accepted
{
    int last = 0, cur = 0;
    bool untrapped = false;
    void init(int v) noexcept
    {
        last = cur = v;
        untrapped = false;
    }
    void set(float v) noexcept
    {
        if (v >= 1.0f)
            cur = 1;
        else if (v <= 0.0f)
            cur = 0;
    }
    void untrap() noexcept { untrapped = true; }
    bool changed() noexcept
    {
        if (untrapped)
        {
            untrapped = false;
            last = cur;
            return true;
        }
        const bool c = cur != last;
        if (c)
            last = cur;
        return c;
    }
    int get() const noexcept { return cur; }
};

// ------------------------------------------------------------------------------------------------ vector helpers

template <bool Neon> inline void copy(float* d, const float* s, int n) noexcept
{
#if APP_FX_HAVE_NEON
    if constexpr (Neon)
    {
        vec::neon::copy(d, s, n);
        return;
    }
#endif
    vec::scalar::copy(d, s, n);
}
template <bool Neon> inline void add(float* d, const float* s, int n) noexcept // d = f32(d + s)
{
#if APP_FX_HAVE_NEON
    if constexpr (Neon)
    {
        vec::neon::add(d, s, n);
        return;
    }
#endif
    vec::scalar::add(d, s, n);
}
template <bool Neon> inline void mul(float* x, float g, int n) noexcept
{
#if APP_FX_HAVE_NEON
    if constexpr (Neon)
    {
        vec::neon::mul(x, g, n);
        return;
    }
#endif
    vec::scalar::mul(x, g, n);
}
template <bool Neon> inline void mulv(float* x, const float* g, int n) noexcept
{
#if APP_FX_HAVE_NEON
    if constexpr (Neon)
    {
        vec::neon::mulv(x, g, n);
        return;
    }
#endif
    vec::scalar::mulv(x, g, n);
}
inline void zero(float* x, int n) noexcept
{
    if (n > 0)
        std::memset(x, 0, sizeof(float) * static_cast<std::size_t>(n));
}

// smooth_mul (send_delay.py, 0xf5b9c), in place on one or two channels: while the ramp runs, x·g[i] (the ramp
// advances once for both channels); settled at 1 → unchanged; |g| < ε → +0; else x·g.
template <bool Neon> inline void smoothMul(SmoothFloat& s, float* a, float* b, int n) noexcept
{
    if (n < 1)
        return;
    if (s.ramping())
    {
        alignas(16) float g[kMaxN];
        s.block(g, n);
        mulv<Neon>(a, g, n);
        if (b != nullptr)
            mulv<Neon>(b, g, n);
        return;
    }
    const float c = s.current();
    if (nearlyEqual(c, 1.0f))
        return;
    if (absf(c) < kEps)
    {
        zero(a, n);
        if (b != nullptr)
            zero(b, n);
        return;
    }
    mul<Neon>(a, c, n);
    if (b != nullptr)
        mul<Neon>(b, c, n);
}

// ------------------------------------------------------------------------------------------------ Ring

class Ring
{
public:
    std::size_t prepare(int len)
    {
        len_ = len > 1 ? len : 1;
        buf_ = std::make_unique<float[]>(static_cast<std::size_t>(len_)); // rt-ok: prepare(), message thread
        std::memset(buf_.get(), 0, sizeof(float) * static_cast<std::size_t>(len_));
        w_ = 0;
        hi_ = 0;
        return sizeof(float) * static_cast<std::size_t>(len_);
    }
    int len() const noexcept { return len_; }
    int w() const noexcept { return w_; }
    void clear() noexcept // O(1): indices to 0, everything written before reads as 0
    {
        w_ = 0;
        hi_ = 0;
    }
    void rewind() noexcept { w_ = 0; } // the reference reset: indices only, the samples are kept
    int wrap(int p) const noexcept
    {
        while (p < 0)
            p += len_;
        while (p >= len_)
            p -= len_;
        return p;
    }
    // n samples from physical index pos (wrapping); a sample not written since the last clear() reads as 0.
    void read(float* out, int pos, int n) const noexcept
    {
        while (n > 0)
        {
            const int m = n < len_ - pos ? n : len_ - pos;
            int valid = hi_ - pos;
            valid = valid < 0 ? 0 : (valid > m ? m : valid);
            if (valid > 0)
                std::memcpy(out, buf_.get() + pos, sizeof(float) * static_cast<std::size_t>(valid));
            zero(out + valid, m - valid);
            out += m;
            n -= m;
            pos = 0;
        }
    }
    float at(int pos) const noexcept { return pos < hi_ ? buf_[static_cast<std::size_t>(pos)] : 0.0f; }
    void write(const float* in, int n) noexcept
    {
        while (n > 0)
        {
            const int m = n < len_ - w_ ? n : len_ - w_;
            std::memcpy(buf_.get() + w_, in, sizeof(float) * static_cast<std::size_t>(m));
            if (w_ + m > hi_)
                hi_ = w_ + m;
            in += m;
            n -= m;
            w_ += m;
            if (w_ == len_)
                w_ = 0;
        }
    }
    float* data() noexcept { return buf_.get(); }
    // Per-sample access of the Reverb's rings (physical index, write index advanced by the caller).
    void put(int pos, float v) noexcept
    {
        buf_[static_cast<std::size_t>(pos)] = v;
        if (pos >= hi_)
            hi_ = pos + 1;
    }
    void setW(int w) noexcept { w_ = w; }
    int highWater() const noexcept { return hi_; }

private:
    std::unique_ptr<float[]> buf_;
    int len_ = 1, w_ = 0, hi_ = 0;
};

// ------------------------------------------------------------------------------------------------ Gate

class Gate // SignalGate (vtable 0x840e40): off 0, on 1, a SmoothFloat ramp of N samples, built closed
{
public:
    void prepare(double fs, int n) noexcept
    {
        r_.prepareSamples(fs, n);
        r_.snap(0.0f);
    }
    void open() noexcept { r_.setTarget(1.0f); }
    void close() noexcept { r_.setTarget(0.0f); }
    void setNow(float v) noexcept { r_.snap(v); }
    bool ramping() const noexcept { return r_.ramping(); }
    bool closing() const noexcept { return r_.ramping() && nearlyEqual(r_.target(), 0.0f); }
    float current() const noexcept { return r_.current(); }
    int remaining() const noexcept { return r_.remaining(); }
    // 0xf521c on one or two channels (the mono module multiplies only its own lane)
    template <bool Neon> void apply(float* a, float* b, int n) noexcept
    {
        const float v = r_.ramping() ? r_.target() : r_.current();
        if (! nearlyEqual(v, 0.0f) && ! nearlyEqual(v, 1.0f))
        {
            zero(a, n);
            if (b != nullptr)
                zero(b, n);
            return;
        }
        smoothMul<Neon>(r_, a, b, n);
    }

private:
    SmoothFloat r_;
};

// ------------------------------------------------------------------------------------------------ Module

// DelayModuleStereo (Ch = 2, insert_delay.py) / DelayModule (Ch = 1, send_delay.py DelayModuleMono).
template <int Ch> class Module
{
public:
    // Message thread: rings of trunc(f32(f32(maxMs · 0.001) · fs)) + 2500 frames (46,600 / 178,900 at 44.1 kHz).
    std::size_t prepare(double fs, float maxMs)
    {
        APP_FX_NOFUSE
        fs_ = fs;
        fsF_ = static_cast<float>(fs);
        maxMs_ = maxMs;
        fullThr_ = static_cast<int>((maxMs * kMilli) * fsF_);
        std::size_t bytes = 0;
        for (auto& r : ring_)
            bytes += r.prepare(fullThr_ + 2500);
        gate_.prepare(fs, rampSamples(fs));
        init();
        return bytes;
    }
    // The constructor state: 50 ms (insert: trunc(f32(f32(50 · 0.001) · fs)); mono: trunc(f32(fs · 0.05))), the gate
    // closed, the rings logically zero.
    void init() noexcept
    {
        APP_FX_NOFUSE
        ms_ = 50.0f;
        D_ = Ch == 2 ? samples(ms_) : static_cast<int>(fsF_ * f32(0.05));
        pend_ = false;
        pms_ = ms_;
        pD_ = D_;
        for (auto& r : ring_)
            r.clear();
        pos_ = 0;
        gate_.setNow(0.0f);
        warm_ = full_ = false;
        cnt_ = 0;
    }
    int samples(float ms) const noexcept
    {
        APP_FX_NOFUSE
        return static_cast<int>((ms * kMilli) * fsF_);
    }
    void setDelay(float ms) noexcept // 0xec9bc with glide = 1
    {
        float x = ms;
        if (! (x > 0.0f))
            x = std::isnan(x) ? x : 0.0f;
        x = maxMs_ > x ? x : maxMs_;
        const float ref = pend_ ? pms_ : ms_;
        if (nearlyEqual(x, ref))
            return;
        pms_ = x;
        pend_ = true;
        pD_ = samples(x);
        gate_.close();
    }
    int chunk() const noexcept { return pend_ ? (D_ < pD_ ? D_ : pD_) : D_; } // 0xecacc
    float ms() const noexcept { return ms_; }
    int delaySamples() const noexcept { return D_; }
    bool pending() const noexcept { return pend_; }
    const Gate& gate() const noexcept { return gate_; }

    // 0xecaf4: n gated samples into out[ch] (no write happens during the request). warmUp = the FX-680/780 quirk.
    template <bool Neon> void read(float* const* out, int n, bool warmUp) noexcept
    {
        int rem = n, done = 0;
        for (;;)
        {
            int c = rem < D_ ? rem : D_;
            if (gate_.closing())
            {
                const int g = gate_.remaining();
                c = g < c ? g : c;
            }
            for (int ch = 0; ch < Ch; ++ch)
                ring_[ch].read(out[ch] + done, pos_, c);
            pos_ = ring_[0].wrap(pos_ + c);
            gate_.template apply<Neon>(out[0] + done, Ch == 2 ? out[1] + done : nullptr, c);
            done += c;
            rem -= c;
            bool cflag = full_;
            if (warmUp && ! full_)
            {
                cnt_ += n; // quirk: the whole request per inner chunk (F079)
                warm_ = D_ <= cnt_;
                if (fullThr_ <= cnt_)
                {
                    full_ = true;
                    cflag = true;
                }
            }
            if (pend_ && ! gate_.ramping() && nearlyEqual(gate_.current(), 0.0f))
            { // gate fully closed: jump to the pending delay
                ms_ = pms_;
                D_ = pD_;
                pos_ = ring_[0].wrap(ring_[0].w() - (D_ - done));
                cflag = full_;
                pend_ = false;
            }
            if (warmUp && ! cflag && ! warm_)
                gate_.close();
            else if (! gate_.closing())
                gate_.open();
            if (rem < 1)
                return;
        }
    }
    void write(const float* const* in, int n) noexcept
    {
        for (int ch = 0; ch < Ch; ++ch)
            ring_[ch].write(in[ch], n);
    }
    // 0xec94c. Reference (staleRing): commit a pending delay, rewind the indices, keep the samples. Sixteen: the rings
    // read as zero afterwards (FX-681/781). warmUp: the gate closes at once (the warm-up gate re-arms); Sixteen
    // (FX-680/780 fix): the gate is open from the reset, the zeroed ring makes the first echo correct.
    void reset(bool staleRing, bool warmUp) noexcept
    {
        if (pend_)
        {
            pend_ = false;
            ms_ = pms_;
            D_ = pD_;
        }
        for (auto& r : ring_)
        {
            if (staleRing)
                r.rewind();
            else
                r.clear();
        }
        pos_ = ring_[0].wrap(-D_);
        gate_.setNow(warmUp ? 0.0f : 1.0f);
        warm_ = full_ = false;
        cnt_ = 0;
    }
    int ringFrames() const noexcept { return ring_[0].len(); }

private:
    double fs_ = 44100.0;
    float fsF_ = 44100.0f, maxMs_ = 1000.0f;
    float ms_ = 50.0f, pms_ = 50.0f;
    int D_ = 2205, pD_ = 2205;
    bool pend_ = false;
    Ring ring_[Ch];
    int pos_ = 0;
    Gate gate_;
    bool warm_ = false, full_ = false;
    int cnt_ = 0, fullThr_ = 44100;
};

// ------------------------------------------------------------------------------------------------ SteppedIir1

class SteppedIir1
{
public:
    enum class Kind : std::uint8_t
    {
        LowPass,
        HighPass
    };
    // ctor 0xf3ce4(c, type, order 1, fs, steps, 1/3 ms, fmin, fmax). LowPass starts at the top step, HighPass at 0.
    void prepare(Kind k, double fs, int steps, double fmin, double fmax) noexcept
    {
        APP_FX_NOFUSE
        kind_ = k;
        fs_ = fs;
        nyq_ = static_cast<float>(static_cast<int>(fs / 2.0));
        steps_ = steps;
        fmin_ = fmin;
        fmax_ = fmax;
        mps_ = (1.0f / static_cast<float>(fs)) * 1000.0f;
        mult_ = std::pow(10.0, std::log10(fmax / fmin) / static_cast<double>(steps - 1));
        init();
    }
    void init() noexcept
    {
        if (kind_ == Kind::LowPass)
        {
            cur_ = tgt_ = steps_ - 1;
            cut_ = tgtCut_ = fmax_;
        }
        else
        {
            cur_ = tgt_ = 0;
            cut_ = tgtCut_ = fmin_;
        }
        acc_ = 0.0f;
        v1_[0] = v1_[1] = y1_[0] = y1_[1] = 0.0f;
        fc_ = -1.0f;
        coefs();
    }
    void setStep(std::uint32_t idx) noexcept // 0xf41ac
    {
        if (idx == 0u)
            idx = 1u;
        const auto steps = static_cast<std::uint32_t>(steps_);
        const int t = static_cast<int>(idx < steps ? idx : steps) - 1;
        if (t != tgt_)
        {
            tgt_ = t;
            tgtCut_ = fmin_ * std::pow(mult_, static_cast<double>(t));
        }
    }
    void resetState() noexcept // 0xf40f8 (the Reverb's damping containers): state 0, at the target step
    {
        v1_[0] = v1_[1] = y1_[0] = y1_[1] = 0.0f;
        cur_ = tgt_;
        acc_ = 0.0f;
        cut_ = tgtCut_;
        coefs();
    }
    void zeroState() noexcept { v1_[0] = v1_[1] = y1_[0] = y1_[1] = 0.0f; }
    bool gliding() const noexcept { return cur_ != tgt_; }
    double cutoff() const noexcept { return cut_; }
    int step() const noexcept { return cur_; }
    int targetStep() const noexcept { return tgt_; }
    float b0() const noexcept { return b0_; }
    float b1() const noexcept { return b1_; }
    float a1() const noexcept { return a1_; }

    // 0xf438c: the glide (one step per 1/3 ms, the block split at the steps), the filter over each chunk.
    template <bool Neon> void process(float* l, float* r, int n) noexcept
    {
        APP_FX_NOFUSE
        if (cur_ == tgt_)
        {
            coefs();
            run<Neon>(l, r, n);
            acc_ = 0.0f;
            return;
        }
        int i0 = 0;
        while (n > 0)
        {
            int c = static_cast<int>((kPeriodMs - acc_) / mps_) + 1;
            c = c < n ? c : n;
            n -= c;
            run<Neon>(l + i0, r + i0, c);
            i0 += c;
            acc_ = acc_ + static_cast<float>(c) * mps_;
            if (acc_ < kPeriodMs)
                continue;
            acc_ = acc_ - kPeriodMs;
            stepOnce();
        }
    }

    // The filter alone over one chunk with the current coefficients (L/R lanes on NEON).
    template <bool Neon> void run(float* l, float* r, int n) noexcept
    {
        APP_FX_NOFUSE
#if APP_FX_HAVE_NEON
        if constexpr (Neon)
        {
            float32x2_t v1 = vld1_f32(v1_), y1 = vld1_f32(y1_);
            for (int j = 0; j < n; ++j)
            {
                const float32x2_t v = vset_lane_f32(r[j], vdup_n_f32(l[j]), 1);
                const float32x2_t pq = vadd_f32(vmul_n_f32(v, b0_), vmul_n_f32(v1, b1_));
                const float32x2_t o = vsub_f32(pq, vmul_n_f32(y1, a1_));
                l[j] = vget_lane_f32(o, 0);
                r[j] = vget_lane_f32(o, 1);
                v1 = v;
                y1 = o;
            }
            vst1_f32(v1_, v1);
            vst1_f32(y1_, y1);
            return;
        }
#endif
        const float b0 = b0_, b1 = b1_, a1 = a1_;
        float* ch[2] = {l, r};
        for (int c = 0; c < 2; ++c)
        {
            float v1 = v1_[c], y1 = y1_[c];
            float* x = ch[c];
            for (int j = 0; j < n; ++j)
            {
                const float v = x[j];
                const float p = b0 * v;
                const float q = b1 * v1;
                const float s = a1 * y1;
                const float o = (p + q) - s;
                x[j] = o;
                v1 = v;
                y1 = o;
            }
            v1_[c] = v1;
            y1_[c] = y1;
        }
    }

private:
    void stepOnce() noexcept // 0xf4218
    {
        if (cur_ < tgt_)
        {
            ++cur_;
            cut_ = cut_ * mult_;
        }
        else if (cur_ > tgt_)
        {
            --cur_;
            cut_ = cut_ / mult_;
        }
        else
            cut_ = tgtCut_;
        coefs();
    }
    void coefs() noexcept // 0xe1b30 (LP) / 0xe1aa0 (HP), fc clamped to [5 Hz, fs/2]
    {
        APP_FX_NOFUSE
        const float s13 = static_cast<float>(cut_);
        const float s15 = s13 <= nyq_ ? s13 : nyq_;
        const float fc = 5.0f <= s15 ? s15 : 5.0f;
        if (fc == fc_)
            return;
        fc_ = fc;
        const double t = std::tan(static_cast<double>(fc) * (kPi / fs_));
        const double d = (t * kInv2Pi) * kTwoPi;
        const double rr = 1.0 / (d + 1.0);
        if (kind_ == Kind::LowPass)
            b0_ = b1_ = static_cast<float>(d * rr);
        else
        {
            b0_ = static_cast<float>(rr);
            b1_ = static_cast<float>(-rr);
        }
        a1_ = static_cast<float>((d - 1.0) * rr);
    }

    Kind kind_ = Kind::LowPass;
    double fs_ = 44100.0, fmin_ = 2000.0, fmax_ = 20000.0, mult_ = 1.0, cut_ = 20000.0, tgtCut_ = 20000.0;
    float nyq_ = 22050.0f, mps_ = 0.0f, acc_ = 0.0f, fc_ = -1.0f;
    int steps_ = 1000, cur_ = 999, tgt_ = 999;
    float b0_ = 0.0f, b1_ = 0.0f, a1_ = 0.0f;
    float v1_[2]{}, y1_[2]{};
};
} // namespace engine::fx::dsp::delay
