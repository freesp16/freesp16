// The analog-filter slot's model kernels (engine/fx/dsp/afs_ladder.h). Kept under engine/fx so that the
// device build compiles them with the effects' IEEE flags (src/engine/CMakeLists.txt: -fno-unsafe-math-optimizations),
// which the NEON ≡ scalar bit-exact contract needs; the slot itself is engine/afs/.
#include "engine/fx/dsp/afs_ladder.h"

#include <cmath>
#include <cstring>

#if APP_FX_HAVE_NEON
#include <arm_neon.h>
#endif

namespace engine::fx::dsp::afs
{
// Odd-phase taps h[2j] of the 63-tap half-band prototype: 0.5·sinc(n/2)·kaiser(63, β = 8)(n), n = 2j − 31, normalised
// so that Σ = 0.5 (generated with numpy, printed to 10 digits; the even phase is the centre tap 0.5).
const float kHalfbandOdd[kPhaseTaps] = {
    -2.401525086e-05f, 1.090362234e-04f, -2.935600618e-04f, 6.381063336e-04f, -1.222075923e-03f, 2.145908431e-03f,
    -3.534414071e-03f, 5.543646654e-03f, -8.376209881e-03f, 1.231558223e-02f, -1.780469905e-02f, 2.563768360e-02f,
    -3.748937911e-02f, 5.772404061e-02f, -1.024425021e-01f, 3.170728513e-01f, 3.170728513e-01f, -1.024425021e-01f,
    5.772404061e-02f, -3.748937911e-02f, 2.563768360e-02f, -1.780469905e-02f, 1.231558223e-02f, -8.376209881e-03f,
    5.543646654e-03f, -3.534414071e-03f, 2.145908431e-03f, -1.222075923e-03f, 6.381063336e-04f, -2.935600618e-04f,
    1.090362234e-04f, -2.401525086e-05f};

namespace
{
constexpr int kUpHist = kPhaseTaps - 1;  // 31
constexpr int kEvenHist = kHalfK - 1;    // 15

struct Taps2 // the upsampler's taps 2·h (exact doubling)
{
    float c[kPhaseTaps];
    Taps2() noexcept
    {
        for (int j = 0; j < kPhaseTaps; ++j)
            c[j] = 2.0f * kHalfbandOdd[j];
    }
};
const Taps2 kUp;

inline void shiftHistory(float* h, int keep) noexcept
{
    std::memmove(h, h + kBlock, sizeof(float) * static_cast<std::size_t>(keep));
}
} // namespace

void HalfbandState::clear() noexcept
{
    std::memset(up, 0, sizeof up);
    std::memset(downOdd, 0, sizeof downOdd);
    std::memset(downEven, 0, sizeof downEven);
}

void HighPass::setCutoff(double hz, double sampleRate) noexcept
{
    // TPT SVF, Q = 1/√2 (Butterworth), in double, rounded to float.
    const double gd = std::tan(3.14159265358979323846 * hz / sampleRate);
    const double R = 0.70710678118654752;
    g = static_cast<float>(gd);
    r2g = static_cast<float>(2.0 * R + gd);
    h = static_cast<float>(1.0 / (1.0 + 2.0 * R * gd + gd * gd));
}

void HighPass::clear() noexcept
{
    s1[0] = s1[1] = s2[0] = s2[1] = 0.0f;
}

// ---------------------------------------------------------------------------------------------------- scalar ----
namespace scalar
{
void upsample(HalfbandState& st, const float* inL, const float* inR, float* out) noexcept
{
    APP_FX_NOFUSE
    alignas(16) float odd[2][kBlock];
    const float* in[2] = {inL, inR};
    for (int c = 0; c < 2; ++c)
    {
        float* h = st.up[c];
        std::memcpy(h + kUpHist, in[c], sizeof(float) * kBlock);
        for (int n = 0; n < kBlock; ++n)
        {
            float a0 = 0.0f, a1 = 0.0f; // even / odd taps: two accumulators (the NEON order)
            for (int j = 0; j < kPhaseTaps; j += 2)
            {
                a0 = a0 + kUp.c[j] * h[kUpHist + n - j];
                a1 = a1 + kUp.c[j + 1] * h[kUpHist + n - j - 1];
            }
            odd[c][n] = a0 + a1;
        }
    }
    for (int n = 0; n < kBlock; ++n) // even phase = the input delayed by K; odd phase = the FIR
    {
        out[4 * n + 0] = st.up[0][kUpHist - kHalfK + n];
        out[4 * n + 1] = st.up[1][kUpHist - kHalfK + n];
        out[4 * n + 2] = odd[0][n];
        out[4 * n + 3] = odd[1][n];
    }
    shiftHistory(st.up[0], kUpHist);
    shiftHistory(st.up[1], kUpHist);
}

void ladder(LadderState& st, const LadderCoefs& cf, const float* in, const float* noise, float* out, bool twoPole) noexcept
{
    APP_FX_NOFUSE
    // L and R in the same sample loop (two independent chains, like the NEON lanes); per channel the operations and
    // their order are the NEON lane's.
    float s0[2] = {st.s[0][0], st.s[0][1]}, s1[2] = {st.s[1][0], st.s[1][1]}, s2[2] = {st.s[2][0], st.s[2][1]},
          s3[2] = {st.s[3][0], st.s[3][1]};
    for (int i = 0; i < kOver; ++i)
    {
        const float G = cf.g[i], G2 = cf.g2[i], G3 = cf.g3[i], inv = cf.inv[i], kio = cf.kio[i];
        for (int c = 0; c < 2; ++c)
        {
            const float x = in[2 * i + c] + noise[2 * i + c];
            // Linear ZDF solve of the feedback node: u = x·inv − kio·((G³s0 + G²s1) + (G·s2 + s3)).
            const float xi = x * inv;
            float p1 = s0[c] * G3;
            p1 = p1 + s1[c] * G2;
            float p2 = s2[c] * G;
            p2 = p2 + s3[c];
            const float P = p1 + p2;
            const float t = xi - P * kio;
            const float u = saturate(t);
            // Forward pass, TPT one-poles: v = G·(x − s), y = v + s, s = y + v.
            float v = u - s0[c];
            v = v * G;
            const float y1 = v + s0[c];
            s0[c] = y1 + v;
            v = saturateInner(y1) - s1[c];
            v = v * G;
            const float y2 = v + s1[c];
            s1[c] = y2 + v;
            v = saturateInner(y2) - s2[c];
            v = v * G;
            const float y3 = v + s2[c];
            s2[c] = y3 + v;
            v = saturateInner(y3) - s3[c];
            v = v * G;
            const float y4 = v + s3[c];
            s3[c] = y4 + v;
            out[2 * i + c] = twoPole ? y2 : y4;
        }
    }
    for (int c = 0; c < 2; ++c)
    {
        st.s[0][c] = s0[c];
        st.s[1][c] = s1[c];
        st.s[2][c] = s2[c];
        st.s[3][c] = s3[c];
    }
}

void downsample(HalfbandState& st, const float* in, float* outL, float* outR) noexcept
{
    APP_FX_NOFUSE
    for (int n = 0; n < kBlock; ++n)
        for (int c = 0; c < 2; ++c)
        {
            st.downEven[c][kEvenHist + n] = in[4 * n + c];
            st.downOdd[c][kUpHist + n] = in[4 * n + 2 + c];
        }
    float* out[2] = {outL, outR};
    for (int c = 0; c < 2; ++c)
    {
        const float* o = st.downOdd[c];
        const float* e = st.downEven[c];
        for (int n = 0; n < kBlock; ++n)
        {
            float a0 = 0.0f, a1 = 0.0f; // even / odd taps: two accumulators (the NEON order)
            for (int j = 0; j < kPhaseTaps; j += 2)
            {
                a0 = a0 + kHalfbandOdd[j] * o[kUpHist + n - j];
                a1 = a1 + kHalfbandOdd[j + 1] * o[kUpHist + n - j - 1];
            }
            float acc = a0 + a1;
            acc = acc + 0.5f * e[n]; // z_even[n − (K − 1)]
            out[c][n] = acc;
        }
        shiftHistory(st.downOdd[c], kUpHist);
        shiftHistory(st.downEven[c], kEvenHist);
    }
}

void highPass(HighPass& hp, float* l, float* r, int n) noexcept
{
    APP_FX_NOFUSE
    float* ch[2] = {l, r};
    for (int c = 0; c < 2; ++c)
    {
        float s1 = hp.s1[c], s2 = hp.s2[c];
        float* x = ch[c];
        for (int i = 0; i < n; ++i)
        {
            float t = hp.r2g * s1;
            t = x[i] - t;
            t = t - s2;
            const float y = t * hp.h;
            const float gy = hp.g * y;
            const float b = gy + s1;
            s1 = gy + b;
            const float gb = hp.g * b;
            const float lo = gb + s2;
            s2 = gb + lo;
            x[i] = y;
        }
        hp.s1[c] = s1;
        hp.s2[c] = s2;
    }
}
} // namespace scalar

// ------------------------------------------------------------------------------------------------------ NEON ----
#if APP_FX_HAVE_NEON
namespace neon
{
namespace
{
inline float32x2_t sat2Inner(float32x2_t w) noexcept
{
    const float32x2_t w2 = vmul_f32(w, w);
    const float32x2_t t = vsub_f32(vdup_n_f32(kSatA), vmul_n_f32(w2, kSatB));
    const float32x2_t u = vsub_f32(vdup_n_f32(1.0f), vmul_f32(w2, t));
    return vmul_f32(w, u);
}
inline float32x2_t sat2(float32x2_t v) noexcept
{
    return sat2Inner(vmin_f32(vmax_f32(v, vdup_n_f32(-kSatClamp)), vdup_n_f32(kSatClamp)));
}
} // namespace

void upsample(HalfbandState& st, const float* inL, const float* inR, float* out) noexcept
{
    std::memcpy(st.up[0] + kUpHist, inL, sizeof(float) * kBlock);
    std::memcpy(st.up[1] + kUpHist, inR, sizeof(float) * kBlock);
    const float* hl = st.up[0];
    const float* hr = st.up[1];
    for (int n = 0; n < kBlock; n += 4)
    {
        // Four independent accumulator chains (L/R × even/odd taps): the A9's add latency, not its issue rate, would
        // bound one chain.
        float32x4_t al0 = vdupq_n_f32(0.0f), al1 = al0, ar0 = al0, ar1 = al0;
        for (int j = 0; j < kPhaseTaps; j += 2)
        {
            al0 = vaddq_f32(al0, vmulq_n_f32(vld1q_f32(hl + kUpHist + n - j), kUp.c[j]));
            ar0 = vaddq_f32(ar0, vmulq_n_f32(vld1q_f32(hr + kUpHist + n - j), kUp.c[j]));
            al1 = vaddq_f32(al1, vmulq_n_f32(vld1q_f32(hl + kUpHist + n - j - 1), kUp.c[j + 1]));
            ar1 = vaddq_f32(ar1, vmulq_n_f32(vld1q_f32(hr + kUpHist + n - j - 1), kUp.c[j + 1]));
        }
        const float32x4_t al = vaddq_f32(al0, al1), ar = vaddq_f32(ar0, ar1);
        float32x4x4_t v;
        v.val[0] = vld1q_f32(hl + kUpHist - kHalfK + n);
        v.val[1] = vld1q_f32(hr + kUpHist - kHalfK + n);
        v.val[2] = al;
        v.val[3] = ar;
        vst4q_f32(out + 4 * n, v);
    }
    shiftHistory(st.up[0], kUpHist);
    shiftHistory(st.up[1], kUpHist);
}

void ladder(LadderState& st, const LadderCoefs& cf, const float* in, const float* noise, float* out, bool twoPole) noexcept
{
    float32x2_t s0 = {st.s[0][0], st.s[0][1]}, s1 = {st.s[1][0], st.s[1][1]}, s2 = {st.s[2][0], st.s[2][1]},
                s3 = {st.s[3][0], st.s[3][1]};
    for (int i = 0; i < kOver; ++i)
    {
        const float G = cf.g[i], G2 = cf.g2[i], G3 = cf.g3[i], inv = cf.inv[i], kio = cf.kio[i];
        const float32x2_t x = vadd_f32(vld1_f32(in + 2 * i), vld1_f32(noise + 2 * i));
        const float32x2_t xi = vmul_n_f32(x, inv);
        float32x2_t p1 = vmul_n_f32(s0, G3);
        p1 = vadd_f32(p1, vmul_n_f32(s1, G2));
        float32x2_t p2 = vmul_n_f32(s2, G);
        p2 = vadd_f32(p2, s3);
        const float32x2_t P = vadd_f32(p1, p2);
        const float32x2_t t = vsub_f32(xi, vmul_n_f32(P, kio));
        const float32x2_t u = sat2(t);
        float32x2_t v = vmul_n_f32(vsub_f32(u, s0), G);
        const float32x2_t y1 = vadd_f32(v, s0);
        s0 = vadd_f32(y1, v);
        v = vmul_n_f32(vsub_f32(sat2Inner(y1), s1), G);
        const float32x2_t y2 = vadd_f32(v, s1);
        s1 = vadd_f32(y2, v);
        v = vmul_n_f32(vsub_f32(sat2Inner(y2), s2), G);
        const float32x2_t y3 = vadd_f32(v, s2);
        s2 = vadd_f32(y3, v);
        v = vmul_n_f32(vsub_f32(sat2Inner(y3), s3), G);
        const float32x2_t y4 = vadd_f32(v, s3);
        s3 = vadd_f32(y4, v);
        vst1_f32(out + 2 * i, twoPole ? y2 : y4);
    }
    vst1_f32(st.s[0], s0);
    vst1_f32(st.s[1], s1);
    vst1_f32(st.s[2], s2);
    vst1_f32(st.s[3], s3);
}

void downsample(HalfbandState& st, const float* in, float* outL, float* outR) noexcept
{
    for (int n = 0; n < kBlock; n += 4)
    {
        const float32x4x4_t v = vld4q_f32(in + 4 * n);
        vst1q_f32(st.downEven[0] + kEvenHist + n, v.val[0]);
        vst1q_f32(st.downEven[1] + kEvenHist + n, v.val[1]);
        vst1q_f32(st.downOdd[0] + kUpHist + n, v.val[2]);
        vst1q_f32(st.downOdd[1] + kUpHist + n, v.val[3]);
    }
    const float* ol = st.downOdd[0];
    const float* orr = st.downOdd[1];
    for (int n = 0; n < kBlock; n += 4) // L and R together: four accumulator chains, as in upsample
    {
        float32x4_t l0 = vdupq_n_f32(0.0f), l1 = l0, r0 = l0, r1 = l0;
        for (int j = 0; j < kPhaseTaps; j += 2)
        {
            l0 = vaddq_f32(l0, vmulq_n_f32(vld1q_f32(ol + kUpHist + n - j), kHalfbandOdd[j]));
            r0 = vaddq_f32(r0, vmulq_n_f32(vld1q_f32(orr + kUpHist + n - j), kHalfbandOdd[j]));
            l1 = vaddq_f32(l1, vmulq_n_f32(vld1q_f32(ol + kUpHist + n - j - 1), kHalfbandOdd[j + 1]));
            r1 = vaddq_f32(r1, vmulq_n_f32(vld1q_f32(orr + kUpHist + n - j - 1), kHalfbandOdd[j + 1]));
        }
        float32x4_t al = vaddq_f32(l0, l1), ar = vaddq_f32(r0, r1);
        al = vaddq_f32(al, vmulq_n_f32(vld1q_f32(st.downEven[0] + n), 0.5f));
        ar = vaddq_f32(ar, vmulq_n_f32(vld1q_f32(st.downEven[1] + n), 0.5f));
        vst1q_f32(outL + n, al);
        vst1q_f32(outR + n, ar);
    }
    for (int c = 0; c < 2; ++c)
    {
        shiftHistory(st.downOdd[c], kUpHist);
        shiftHistory(st.downEven[c], kEvenHist);
    }
}

void highPass(HighPass& hp, float* l, float* r, int n) noexcept
{
    float32x2_t s1 = {hp.s1[0], hp.s1[1]}, s2 = {hp.s2[0], hp.s2[1]};
    for (int i = 0; i < n; ++i)
    {
        float32x2_t x = vdup_n_f32(0.0f);
        x = vld1_lane_f32(l + i, x, 0);
        x = vld1_lane_f32(r + i, x, 1);
        float32x2_t t = vmul_n_f32(s1, hp.r2g);
        t = vsub_f32(x, t);
        t = vsub_f32(t, s2);
        const float32x2_t y = vmul_n_f32(t, hp.h);
        const float32x2_t gy = vmul_n_f32(y, hp.g);
        const float32x2_t b = vadd_f32(gy, s1);
        s1 = vadd_f32(gy, b);
        const float32x2_t gb = vmul_n_f32(b, hp.g);
        const float32x2_t lo = vadd_f32(gb, s2);
        s2 = vadd_f32(gb, lo);
        vst1_lane_f32(l + i, y, 0);
        vst1_lane_f32(r + i, y, 1);
    }
    vst1_f32(hp.s1, s1);
    vst1_f32(hp.s2, s2);
}
} // namespace neon
#endif
} // namespace engine::fx::dsp::afs
