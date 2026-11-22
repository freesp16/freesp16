// The effects DSP kit (engine/fx/dsp/): every NEON kernel against its scalar reference (bit for bit, every length
// 0…67, aligned and unaligned), the shell's high-pass coefficients (FX-028), ramps (N_shell, N_mute, accumulated
// steps), the delay ring (reads, wrap, O(1) logical clear), filters and the oscillator.
#include "engine/fx/dsp/DelayLine.h"
#include "engine/fx/dsp/Filters.h"
#include "engine/fx/dsp/Interp.h"
#include "engine/fx/dsp/Oscillator.h"
#include "engine/fx/dsp/Ramps.h"
#include "engine/fx/dsp/Vec.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <random>
#include <vector>

using namespace engine::fx::dsp;

namespace
{
std::vector<float> noise(int n, unsigned seed, float scale = 1.0f)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> d(-scale, scale);
    std::vector<float> v(static_cast<std::size_t>(n) + 1);
    for (auto& x : v)
        x = d(rng);
    return v;
}
bool sameBits(const float* a, const float* b, int n)
{
    for (int i = 0; i < n; ++i)
        if (bitsOf(a[i]) != bitsOf(b[i]))
            return false;
    return true;
}
} // namespace

TEST_CASE("fx.dsp: N_shell and N_mute per sample rate", "[fx][dsp]")
{
    CHECK(shellFadeSamples(44100.0) == 147);
    CHECK(shellFadeSamples(48000.0) == 160);
    CHECK(shellFadeSamples(96000.0) == 320);
    CHECK(muteFadeSamples(44100.0) == 132);
    CHECK(muteFadeSamples(48000.0) == 144);
    CHECK(muteFadeSamples(96000.0) == 288);
}

#if APP_FX_HAVE_NEON
TEST_CASE("fx.dsp: NEON vector kernels equal the scalar references bit for bit", "[fx][dsp][simd]")
{
    for (int n = 0; n <= 67; ++n)
        for (int off = 0; off < 2; ++off)
        {
            INFO("n " << n << " offset " << off);
            const auto x = noise(n + 1, 1u + static_cast<unsigned>(n)),
                       c = noise(n + 1, 77u + static_cast<unsigned>(n));
            const auto d = noise(n + 1, 3u), w = noise(n + 1, 4u);
            auto a = x, b = x;
            vec::scalar::copy(a.data() + off, c.data(), n);
            vec::neon::copy(b.data() + off, c.data(), n);
            CHECK(sameBits(a.data(), b.data(), n + 1));
            a = b = x;
            vec::scalar::mul(a.data() + off, 0.3712f, n);
            vec::neon::mul(b.data() + off, 0.3712f, n);
            CHECK(sameBits(a.data(), b.data(), n + 1));
            a = b = x;
            vec::scalar::mulv(a.data() + off, d.data(), n);
            vec::neon::mulv(b.data() + off, d.data(), n);
            CHECK(sameBits(a.data(), b.data(), n + 1));
            a = b = x;
            vec::scalar::add(a.data() + off, c.data(), n);
            vec::neon::add(b.data() + off, c.data(), n);
            CHECK(sameBits(a.data(), b.data(), n + 1));
            a = b = x;
            vec::scalar::mix(a.data() + off, c.data(), 0.61f, 0.39f, n);
            vec::neon::mix(b.data() + off, c.data(), 0.61f, 0.39f, n);
            CHECK(sameBits(a.data(), b.data(), n + 1));
            a = b = x;
            vec::scalar::mixv(a.data() + off, c.data(), d.data(), w.data(), n);
            vec::neon::mixv(b.data() + off, c.data(), d.data(), w.data(), n);
            CHECK(sameBits(a.data(), b.data(), n + 1));
            CHECK(bitsOf(vec::scalar::peakAbs(x.data() + off, n)) == bitsOf(vec::neon::peakAbs(x.data() + off, n)));
        }
}
#endif

TEST_CASE("fx.dsp: the mix is two separately rounded products (FX-030)", "[fx][dsp]")
{
    // Values whose fused and non-fused results differ: x·d + c·w with a product needing more than 24 bits.
    float x[4] = {1.0f + 1.0f / 4096.0f, 0.1f, 0.3f, 0.7f};
    const float c[4] = {1.0f - 1.0f / 8192.0f, 0.2f, 0.9f, 0.123f};
    const float d = 1.0f + 1.0f / 2048.0f, w = -(1.0f - 1.0f / 8192.0f);
    float expect[4];
    for (int i = 0; i < 4; ++i)
    {
        volatile float p = x[i] * d; // volatile: rounded to float here
        volatile float q = c[i] * w;
        expect[i] = p + q;
    }
    vec::mix(x, c, d, w, 4);
    CHECK(sameBits(x, expect, 4));
}

TEST_CASE("fx.dsp: the shell high-pass coefficients and the NEON path (FX-028)", "[fx][dsp][filter]")
{
    Hpf1 h;
    h.prepare(44100.0);
    h.setCutoff(10.0f);
    CHECK(h.b0() == Catch::Approx(0.999288).margin(1e-6));
    CHECK(h.b1() == -h.b0());
    CHECK(h.a1() == Catch::Approx(-0.998576).margin(1e-6));
    h.setCutoff(1.0f); // clamped to 5 Hz
    Hpf1 five;
    five.prepare(44100.0);
    five.setCutoff(5.0f);
    CHECK(h.b0() == five.b0());
    h.setCutoff(10.0f);
    Hpf1 s = h;
    auto l = noise(64, 5), r = noise(64, 6);
    auto l2 = l, r2 = r;
    for (int b = 0; b < 20; ++b)
    {
        h.process(l.data(), r.data(), 64);
        s.processScalar(l2.data(), r2.data(), 64);
    }
    CHECK(sameBits(l.data(), l2.data(), 64));
    CHECK(sameBits(r.data(), r2.data(), 64));
    // DC is removed
    Hpf1 dc;
    dc.prepare(48000.0);
    dc.setCutoff(10.0f);
    float a[64], b[64];
    for (int blk = 0; blk < 1500; ++blk)
    {
        std::fill(a, a + 64, 0.5f);
        std::fill(b, b + 64, -0.5f);
        dc.process(a, b, 64);
    }
    CHECK(std::fabs(a[63]) < 1e-4f);
    CHECK(std::fabs(b[63]) < 1e-4f);
}

TEST_CASE("fx.dsp: Biquad2 NEON equals scalar; designs have the right DC/Nyquist gains", "[fx][dsp][filter]")
{
    using T = BiquadCoefs::Type;
    for (T t : {T::LowPass, T::HighPass, T::BandPass, T::Notch, T::AllPass, T::Peaking, T::LowShelf, T::HighShelf})
    {
        Biquad2 a, b;
        a.setCoefs(BiquadCoefs::design(t, 48000.0, 1200.0, 0.9, 6.0));
        b.setCoefs(a.coefs());
        auto l = noise(64, 9), r = noise(64, 10);
        auto l2 = l, r2 = r;
        for (int k = 0; k < 8; ++k)
        {
            a.process(l.data(), r.data(), 64);
            b.processScalar(l2.data(), r2.data(), 64);
        }
        CHECK(sameBits(l.data(), l2.data(), 64));
        CHECK(sameBits(r.data(), r2.data(), 64));
    }
    const auto lp = BiquadCoefs::design(T::LowPass, 44100.0, 1000.0, 0.707);
    CHECK((lp.b0 + lp.b1 + lp.b2) / (1.0f + lp.a1 + lp.a2) == Catch::Approx(1.0).margin(1e-4));
    const auto hp = BiquadCoefs::design(T::HighPass, 44100.0, 1000.0, 0.707);
    CHECK((hp.b0 + hp.b1 + hp.b2) == Catch::Approx(0.0).margin(1e-6));
}

TEST_CASE("fx.dsp: OnePole and Svf pass/stop DC as their modes say", "[fx][dsp][filter]")
{
    OnePole op;
    op.setCutoff(44100.0, 200.0);
    Svf sv;
    sv.setParams(44100.0, 200.0, 0.707);
    float lp[64], hp[64], r1[64], r2[64], s[64], s2[64];
    for (int blk = 0; blk < 200; ++blk)
    {
        std::fill(lp, lp + 64, 1.0f);
        std::fill(hp, hp + 64, 1.0f);
        std::fill(r1, r1 + 64, 1.0f);
        std::fill(r2, r2 + 64, 1.0f);
        std::fill(s, s + 64, 1.0f);
        std::fill(s2, s2 + 64, 1.0f);
        op.lowPass(lp, r1, 64);
        sv.process(Svf::Mode::LowPass, s, s2, 64);
    }
    CHECK(lp[63] == Catch::Approx(1.0f).margin(1e-4));
    CHECK(s[63] == Catch::Approx(1.0f).margin(1e-4));
    OnePole op2;
    op2.setCutoff(44100.0, 200.0);
    Svf hv;
    hv.setParams(44100.0, 200.0, 0.707);
    for (int blk = 0; blk < 200; ++blk)
    {
        std::fill(hp, hp + 64, 1.0f);
        std::fill(r2, r2 + 64, 1.0f);
        std::fill(s, s + 64, 1.0f);
        std::fill(s2, s2 + 64, 1.0f);
        op2.highPass(hp, r2, 64);
        hv.process(Svf::Mode::HighPass, s, s2, 64);
    }
    CHECK(std::fabs(hp[63]) < 1e-4f);
    CHECK(std::fabs(s[63]) < 1e-4f);
}

TEST_CASE("fx.dsp: SmoothFloat ramps are accumulated, first sample = start (reference SmoothFloat)", "[fx][dsp][ramp]")
{
    SmoothFloat s;
    s.prepare(44100.0);
    s.setTime(3.3333333f);
    CHECK(s.samples() == 147);
    s.snap(0.0f);
    s.setTarget(1.0f);
    float g[64];
    s.block(g, 64);
    CHECK(g[0] == 0.0f);
    const float step = (1.0f - 0.0f) * static_cast<float>(1.0 / 147.0);
    float c = 0.0f;
    for (int i = 0; i < 64; ++i)
    {
        CHECK(g[i] == c);
        c += step;
    }
    s.block(g, 64);
    s.block(g, 64); // samples 128…191: the ramp ends at 147
    CHECK(g[18] != 1.0f);
    CHECK(g[19] == 1.0f);
    CHECK_FALSE(s.ramping());
    s.setTarget(1.0f + 1e-8f); // near the target: ignored
    CHECK_FALSE(s.ramping());
}

TEST_CASE("fx.dsp: MuteSmoother mute, unmute and the bypass crossfade (N_mute)", "[fx][dsp][ramp]")
{
    MuteSmoother m;
    m.prepare(132, false);
    m.setMuted(true);
    std::vector<float> l(256, 1.0f), r(256, 1.0f);
    for (int b = 0; b < 4; ++b)
        m.process(l.data() + 64 * b, r.data() + 64 * b, 64);
    CHECK(l[0] == 1.0f);
    CHECK(l[131] > 0.0f);
    CHECK(l[132] == 0.0f);
    CHECK(m.state() == MuteSmoother::Closed);
    MuteSmoother x;
    x.prepare(132, false);
    x.setMuted(true); // bypass on: processed → dry over 132 samples, then exactly dry
    float p[64], q[64], dl[64], dr[64];
    for (int b = 0; b < 3; ++b)
    {
        std::fill(p, p + 64, 0.25f);
        std::fill(q, q + 64, 0.25f);
        std::fill(dl, dl + 64, 1.0f);
        std::fill(dr, dr + 64, -1.0f);
        x.crossfade(p, q, dl, dr, 64);
    }
    CHECK(x.state() == MuteSmoother::Closed);
    CHECK(p[63] == 1.0f);
    CHECK(q[63] == -1.0f);
}

TEST_CASE("fx.dsp: DelayLine reads, wraps and clears in O(1)", "[fx][dsp][delay]")
{
    DelayLine d;
    CHECK(d.prepare(1000) == 2 * sizeof(float) * 1004);
    std::vector<float> in(64);
    for (int b = 0; b < 40; ++b) // 2560 samples: wraps the 1004-sample ring twice
    {
        for (int i = 0; i < 64; ++i)
            in[static_cast<std::size_t>(i)] = static_cast<float>(b * 64 + i);
        float out[64], outR[64];
        d.readBlock(out, outR, 300, 64); // sample t − 300 + j
        if (b * 64 >= 300)
            for (int i = 0; i < 64; ++i)
                CHECK(out[i] == static_cast<float>(b * 64 - 300 + i));
        d.writeBlock(in.data(), in.data(), 64);
    }
    CHECK(d.readL(1) == 2559.0f);
    float l = 0, r = 0;
    d.readLinear(10.25f, l, r);
    CHECK(l == Catch::Approx(2560.0f - 10.25f)); // age 1 = 2559
    d.readHermite(10.5f, l, r);
    CHECK(l == Catch::Approx(2560.0f - 10.5f));
    d.clear(); // logical: every old sample reads 0
    CHECK(d.readL(1) == 0.0f);
    d.write(5.0f, 6.0f);
    CHECK(d.readL(1) == 5.0f);
    CHECK(d.readR(1) == 6.0f);
    CHECK(d.readL(2) == 0.0f);
    float out[64], outR[64];
    d.readBlock(out, outR, 64, 64);
    CHECK(out[62] == 0.0f);
    CHECK(out[63] == 5.0f);
    CHECK(d.readL(2000) == 0.0f);
}

TEST_CASE("fx.dsp: interpolation and the table oscillator", "[fx][dsp][osc]")
{
    CHECK(interp::linear(1.0f, 3.0f, 0.25f) == 1.5f);
    CHECK(interp::hermite(0.0f, 1.0f, 2.0f, 3.0f, 0.5f) == Catch::Approx(1.5f));
    Oscillator o;
    o.setFrequency(48000.0, 1000.0);
    float out[48];
    o.render(out, 48); // exactly one cycle
    double maxErr = 0;
    for (int i = 0; i < 48; ++i)
        maxErr = std::max(maxErr, std::fabs(static_cast<double>(out[i]) - std::sin(2.0 * 3.14159265358979 * i / 48.0)));
    CHECK(maxErr < 2e-5);
    CHECK(std::min(o.phase(), 1.0 - o.phase()) < 1e-6); // back at the start (mod 1)
    for (auto shape :
         {Oscillator::Shape::Triangle, Oscillator::Shape::SawUp, Oscillator::Shape::SawDown, Oscillator::Shape::Square})
    {
        o.setShape(shape);
        o.retrigger(0.0);
        float y[512];
        o.render(y, 512);
        for (float v : y)
            CHECK(std::fabs(v) <= 1.2f);
    }
    o.setShape(Oscillator::Shape::Triangle);
    o.setBandLimited(false);
    o.setPhase(0.25);
    CHECK(o.value() == Catch::Approx(1.0f).margin(1e-6));
    o.setFrequency(48000.0, 1.0);
    o.retrigger(0.5);
    o.advance(64 * 375); // 0.5 s at 1 Hz
    CHECK(std::min(o.phase(), 1.0 - o.phase()) < 1e-5);
}
