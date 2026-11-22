// 2-band EQ (spec 02-features/effects/parametric-eq.md): parameter laws and the app enum, the Zölzer
// boost forms (FX-421/422: a cut is narrower than a boost), the shelf/peak limits (FX-423), glide step counts against the
// research model peq.py, the type swap's output gain (FX-426), the quirk profile (quirk_eq2: FX-480 start-up dip,
// FX-056 trap) and the NEON path against its scalar reference (neon_vs_scalar_eq2: bit for bit; skipped without NEON —
// scripts/neon-tests.sh runs it under qemu-arm). The goldens are tests/golden/eq2{,_48k}.
#include "core/util/Realtime.h"
#include "engine/fx/EffectRegistry.h"
#include "engine/fx/FxShell.h"
#include "engine/fx/dsp/filter_fdlibm.h"
#include "engine/fx/effects/ParametricEq.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <complex>
#include <cstring>
#include <vector>

using engine::fx::effects::ParametricEq;
using Catch::Approx;

namespace
{
constexpr int N = engine::kBlockFrames;

engine::fx::ProcessContext ctx(double fs = 44100.0)
{
    engine::fx::ProcessContext c{fs, 120.0, false, 0u, {}, 0};
    c.triggerOffset.fill(-1);
    return c;
}

struct Signal
{
    std::vector<float> l, r;
};
Signal constant(int blocks, float v)
{
    return {std::vector<float>(static_cast<std::size_t>(blocks * N), v),
            std::vector<float>(static_cast<std::size_t>(blocks * N), v)};
}
Signal sine(int blocks, double hz = 1000.0, float a = 0.5f, double fs = 44100.0)
{
    Signal s;
    for (int i = 0; i < blocks * N; ++i)
    {
        s.l.push_back(a * static_cast<float>(std::sin(2.0 * 3.141592653589793 * hz * i / fs)));
        s.r.push_back(-s.l.back());
    }
    return s;
}
Signal noise(int blocks, std::uint32_t seed, float a = 0.5f)
{
    Signal s;
    std::uint32_t x = seed;
    auto next = [&]
    {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        return a * (static_cast<float>(x >> 8) * (2.0f / 16777216.0f) - 1.0f);
    };
    for (int i = 0; i < blocks * N; ++i)
    {
        s.l.push_back(next());
        s.r.push_back(next());
    }
    return s;
}

void defaults(ParametricEq& e)
{
    for (int k = 0; k < 6; ++k)
        e.setParam(k, ParametricEq::kDefaults[k]);
    e.setOption(ParametricEq::kOptType1, ParametricEq::kAppLoShelf);
    e.setOption(ParametricEq::kOptType2, ParametricEq::kAppHiShelf);
}

void block(ParametricEq& e, float* l, float* r, bool scalar = false)
{
    const auto c = ctx();
    if (scalar)
        e.processScalar(l, r, c);
    else
        e.process(l, r, c);
}

Signal run(ParametricEq& e, const Signal& in)
{
    Signal out = in;
    for (std::size_t b = 0; b < in.l.size() / N; ++b)
        block(e, out.l.data() + b * N, out.r.data() + b * N);
    return out;
}

bool sameBits(const std::vector<float>& a, const std::vector<float>& b)
{
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

double magDb(const engine::fx::dsp::filt::Coefs& c, double f, double fs)
{
    const std::complex<double> z1 = std::polar(1.0, -2.0 * 3.141592653589793 * f / fs), z2 = z1 * z1;
    const double b0 = c.b0, b1 = c.b1, b2 = c.b2, a1 = c.a1, a2 = c.a2;
    const auto h = (b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2);
    return 20.0 * std::log10(std::abs(h));
}

// half-gain bandwidth in octaves of a peak at fc (gain dB beyond half the peak's)
double halfGainOctaves(const engine::fx::dsp::filt::Coefs& c, double fc, double peakDb, double fs)
{
    const double half = peakDb / 2.0;
    auto inside = [&](double f) { return peakDb > 0 ? magDb(c, f, fs) >= half : magDb(c, f, fs) <= half; };
    auto edge = [&](double lo, double hi) // inside(lo) != inside(hi)
    {
        for (int i = 0; i < 80; ++i)
        {
            const double mid = std::sqrt(lo * hi);
            (inside(mid) == inside(lo) ? lo : hi) = mid;
        }
        return std::sqrt(lo * hi);
    };
    return std::log2(edge(fc, fs * 0.49) / edge(10.0, fc));
}

engine::fx::FxShell* makeShell(engine::fx::FxShell& shell, ParametricEq& e, double fs, std::uint32_t profile,
                               float* sl, float* sr)
{
    const auto* d = engine::fx::EffectRegistry::builtin().find("eq2");
    REQUIRE(d != nullptr);
    e.prepare(fs, N);
    shell.prepare(e, d->shell, fs, engine::fx::SlotKind::Insert, sl, sr);
    shell.setQuirkProfile(profile);
    for (int k = 0; k < 6; ++k)
        shell.setParam(k, ParametricEq::kDefaults[k]);
    shell.setOption(ParametricEq::kOptType1, ParametricEq::kAppLoShelf);
    shell.setOption(ParametricEq::kOptType2, ParametricEq::kAppHiShelf);
    shell.activate();
    return &shell;
}
} // namespace

TEST_CASE("fx.eq2: parameter laws, the app enum and the descriptor (FX-420/424/425, FX-069/461)", "[fx][eq2]")
{
    CHECK(ParametricEq::freqOf(0.0f, false) == 20.0f);
    CHECK(ParametricEq::freqOf(1.0f, true) == 20000.0f);
    CHECK(ParametricEq::freqOf(0.566f, true) == Approx(997.77).epsilon(1e-4));
    CHECK(ParametricEq::qOf(6.0f / 99.0f) == Approx(0.7f).epsilon(1e-6));
    CHECK(ParametricEq::qOf(1.0f) == 10.0f);
    CHECK(ParametricEq::gainDbOf(2.0f / 3.0f) == 0.0f);
    CHECK(ParametricEq::gainOf(-12.0f) == Approx(0.2511886).epsilon(1e-6));
    // fdlibm powf ≠ std::pow on some grid points (≤ 6 ulp, FX-054); both within 1e-6 relative
    int differ = 0;
    for (int i = 0; i <= 1000; ++i)
    {
        const float v = static_cast<float>(i) / 1000.0f;
        const float a = ParametricEq::freqOf(v, true), b = ParametricEq::freqOf(v, false);
        differ += a != b;
        CHECK(std::fabs(a - b) <= 1e-6f * a);
    }
    CHECK(differ > 0);
    CHECK(engine::fx::dsp::fdlibm::log10f(1000.0f) == 3.0f);

    // UI-order enum (HPF, LPF, PEAK, HI-SHELF, LO-SHELF) → the reference's band types
    const int appTypes[] = {ParametricEq::kAppHpf, ParametricEq::kAppLpf, ParametricEq::kAppPeak,
                            ParametricEq::kAppHiShelf, ParametricEq::kAppLoShelf};
    const int refTypes[] = {ParametricEq::kHighPass, ParametricEq::kLowPass, ParametricEq::kPeak,
                            ParametricEq::kHighShelf, ParametricEq::kLowShelf};
    for (int i = 0; i < 5; ++i)
    {
        ParametricEq e;
        e.prepare(44100.0, N);
        defaults(e);
        e.setOption(ParametricEq::kOptType1, appTypes[i]);
        e.setOption(ParametricEq::kOptType2, appTypes[(i + 2) % 5]);
        e.reset();
        float l[N] = {}, r[N] = {};
        block(e, l, r);
        CHECK(e.band(0).type == refTypes[i]);
        CHECK(e.band(1).type == refTypes[(i + 2) % 5]);
    }
    const auto* d = engine::fx::EffectRegistry::builtin().find("eq2");
    REQUIRE(d != nullptr);
    CHECK_FALSE(d->shell.wetHighPass);
    CHECK(d->shell.mixLaw == static_cast<std::uint8_t>(engine::fx::MixLaw::WetOnly));
    CHECK(d->params[0] == "fx.eq2.b1q"); // page order (FX-067)
}

TEST_CASE("fx.eq2: Zölzer boost forms — a cut is narrower than a boost (FX-421/422), limits (FX-423)", "[fx][eq2]")
{
    using engine::fx::dsp::eq2::Shelf;
    struct Row
    {
        double q, boost, cut;
    };
    for (const Row& row : {Row{0.7, 3.30, 1.01}, Row{2.0, 1.38, 0.36}, Row{7.0, 0.41, 0.10}})
    {
        INFO("Q " << row.q);
        Shelf p;
        p.init(true, 44100.0);
        p.configure(1000.0f, row.q, 12.0, false);
        CHECK(halfGainOctaves(p.iir.c, 1000.0, 12.0, 44100.0) == Approx(row.boost).margin(0.02));
        CHECK(magDb(p.iir.c, 1000.0, 44100.0) == Approx(12.0).margin(1e-3));
        p.configure(1000.0f, row.q, -12.0, false);
        CHECK(halfGainOctaves(p.iir.c, 1000.0, -12.0, 44100.0) == Approx(row.cut).margin(0.02));
        CHECK(magDb(p.iir.c, 1000.0, 44100.0) == Approx(-12.0).margin(1e-3));
    }
    // low shelf: gain V at DC, 1 at Nyquist
    Shelf s;
    s.init(false, 48000.0);
    s.configure(500.0f, 0.7, 9.0, false);
    CHECK(magDb(s.iir.c, 1.0, 48000.0) == Approx(9.0).margin(1e-3));
    CHECK(magDb(s.iir.c, 23999.0, 48000.0) == Approx(0.0).margin(1e-3));
    // FX-423: values outside fc 1 Hz…Nyquist, Q 0.01…20, gain −80…+40 dB (and V − 1 outside −1…100) keep the previous
    // ones, so the coefficients do not change
    const auto before = s.iir.c;
    s.configure(0.5f, 0.001, 50.0, false);
    CHECK(std::memcmp(&before, &s.iir.c, sizeof before) == 0);
    CHECK(s.fc == 500.0f);
    CHECK(s.Q == 0.7);
    CHECK(s.gdb == 9.0);
    s.configure(30000.0f, 25.0, 45.0, false); // (−90 dB would be ignored as a gain but accepted as V − 1 > −1)
    CHECK(std::memcmp(&before, &s.iir.c, sizeof before) == 0);
    s.configure(500.0f, 2.0, 9.0, false); // an accepted Q changes them
    CHECK(std::memcmp(&before, &s.iir.c, sizeof before) != 0);
}

TEST_CASE("fx.eq2: glide step counts equal the research model exactly; gated jumps (FX-425)", "[fx][eq2]")
{
    // band 1 peak; (freq, gain, Q targets) → after one block: frequency / Q / gain steps, gate fading, fc, gain dB —
    // peq.py at 44.1 kHz: insert defaults + BAND1TYPE 2, reset, 4 silent blocks, the change, one silent block.
    struct Row
    {
        float f, g, q;
        int fs, qs, gs;
        bool gated;
        float fc, gc;
    };
    const Row rows[] = {
        {0.6f, -1, -1, 7, 0, 0, false, 1054.4595947265625f, 0.0f},
        {0.8f, -1, -1, 59, 1, 1, true, 997.7688598632812f, 0.0f},
        {-1, 0.5333f, -1, 0, 0, 149, false, 997.7688598632812f, -0.07999999821186066f},
        {-1, 1.0f, -1, 1, 1, 376, true, 997.7688598632812f, 0.0f},
        {-1, -1, 0.2424f, 0, 26, 0, false, 997.7688598632812f, 0.0f},
        {-1, -1, 0.0f, 0, 41, 0, false, 997.7688598632812f, 0.0f},
        {0.3f, 0.9f, 0.5f, 67, 43, 263, true, 997.7688598632812f, 0.0f},
    };
    float z[N] = {}, zr[N] = {};
    for (const auto& row : rows)
    {
        INFO("f " << row.f << " g " << row.g << " q " << row.q);
        ParametricEq e;
        e.prepare(44100.0, N);
        e.setQuirkProfile(engine::fx::kQuirkProfileReference);
        e.setOption(ParametricEq::kOptParityMaths, 1);
        defaults(e);
        e.setOption(ParametricEq::kOptType1, ParametricEq::kAppPeak);
        e.reset();
        for (int b = 0; b < 4; ++b)
            block(e, z, zr);
        if (row.f >= 0)
            e.setParam(0, row.f);
        if (row.g >= 0)
            e.setParam(1, row.g);
        if (row.q >= 0)
            e.setParam(2, row.q);
        block(e, z, zr);
        const auto v = e.band(0);
        CHECK(v.fSteps == row.fs);
        CHECK(v.qSteps == row.qs);
        CHECK(v.gSteps == row.gs);
        CHECK(v.fc == row.fc);
        CHECK(v.gc == row.gc);
        (void) row.gated; // the gated jumps are compared sample by sample in the goldens (freq_jumps, gain_jumps)
    }
}

TEST_CASE("fx.eq2: the type swap sets HI-SHELF/LPF/HPF gains at once, LO-SHELF/PEAK ramp to 1 (FX-426)", "[fx][eq2]")
{
    ParametricEq e;
    e.prepare(44100.0, N);
    defaults(e);
    e.setParam(1, 0.8f); // band 1 +6 dB
    e.reset();
    float l[N] = {}, r[N] = {};
    for (int b = 0; b < 4; ++b)
        block(e, l, r);
    CHECK(e.band(0).gain == 1.0f); // low shelf: output gain 1
    e.setOption(ParametricEq::kOptType1, ParametricEq::kAppHpf);
    for (int b = 0; b < 3; ++b) // 132-sample fade-out, swap inside block 2
        block(e, l, r);
    CHECK(e.band(0).type == ParametricEq::kHighPass);
    CHECK(e.band(0).gain == ParametricEq::gainOf(ParametricEq::gainDbOf(0.8f)));
    e.setOption(ParametricEq::kOptType1, ParametricEq::kAppPeak);
    for (int b = 0; b < 3; ++b)
        block(e, l, r);
    CHECK(e.band(0).type == ParametricEq::kPeak);
    CHECK(e.band(0).gain < ParametricEq::gainOf(6.0f)); // ramping (44 samples) towards 1
    for (int b = 0; b < 2; ++b)
        block(e, l, r);
    CHECK(e.band(0).gain == 1.0f);
}

TEST_CASE("fx.eq2: quirk_eq2 start-up dip (FX-480) and the first-value trap (FX-056)", "[fx][eq2][quirk]")
{
    // FX-480 through the shell, DC 0.5 at the defaults: the reference dips to 0.6544 at sample 76 (44.1 kHz) and to
    // 0.6536 at sample 83 (48 kHz); Sixteen stays at the input level.
    struct Rate
    {
        double fs;
        int at;
        double level;
    };
    for (const Rate& rate : {Rate{44100.0, 76, 0.6544}, Rate{48000.0, 83, 0.6536}})
    {
        INFO("fs " << rate.fs);
        for (std::uint32_t profile : {engine::fx::kQuirkProfileReference, engine::fx::kQuirkProfileSixteen})
        {
            ParametricEq e;
            engine::fx::FxShell shell;
            alignas(16) float sl[N], sr[N];
            makeShell(shell, e, rate.fs, profile, sl, sr);
            auto s = constant(8, 0.5f);
            const auto c = ctx(rate.fs);
            for (int b = 0; b < 8; ++b)
                shell.process(s.l.data() + b * N, s.r.data() + b * N, c);
            int at = 0;
            for (int i = 0; i < 8 * N; ++i)
                if (s.l[static_cast<std::size_t>(i)] < s.l[static_cast<std::size_t>(at)])
                    at = i;
            const double minLevel = static_cast<double>(s.l[static_cast<std::size_t>(at)]) / 0.5;
            if (profile == engine::fx::kQuirkProfileReference)
            {
                CHECK(at == rate.at);
                CHECK(minLevel == Approx(rate.level).margin(1e-4));
            }
            else
                CHECK(minLevel > 0.999);
        }
    }

    // Sixteen = Reference from block 3 on at the defaults (spec test note), and after the filters' start transient
    // has decayed for any setting (Sixteen filters block 0, the reference starts at block 1).
    for (int setting = 0; setting < 2; ++setting)
    {
        ParametricEq ref, six;
        for (auto* e : {&ref, &six})
            e->prepare(44100.0, N);
        ref.setQuirkProfile(engine::fx::kQuirkProfileReference);
        six.setQuirkProfile(engine::fx::kQuirkProfileSixteen);
        for (auto* e : {&ref, &six})
        {
            defaults(*e);
            if (setting == 1)
            {
                e->setOption(ParametricEq::kOptType1, ParametricEq::kAppPeak);
                e->setParam(1, 0.9333f); // +12 dB
                e->setParam(2, 0.192f);  // Q 2
            }
            e->reset();
        }
        const auto in = sine(40);
        const auto a = run(ref, in), b = run(six, in);
        const int from = setting == 0 ? 3 * N : 20 * N;
        double worst = 0, peak = 0;
        for (int i = from; i < 40 * N; ++i)
        {
            worst = std::fmax(worst, std::fabs(a.l[static_cast<std::size_t>(i)] - b.l[static_cast<std::size_t>(i)]));
            peak = std::fmax(peak, std::fabs(a.l[static_cast<std::size_t>(i)]));
        }
        INFO("setting " << setting);
        CHECK(worst <= 1e-5 * peak);
        double first = 0; // block 0: the reference passes it unfiltered (gated), Sixteen equalises it
        for (int i = 0; i < N; ++i)
            first = std::fmax(first, std::fabs(a.l[static_cast<std::size_t>(i)] - b.l[static_cast<std::size_t>(i)]));
        CHECK(first > 1e-3);
    }

    // FX-056: a first value equal to the constructor's "last" (0.5) is swallowed by the reference only.
    ParametricEq rt, st;
    for (auto* e : {&rt, &st})
        e->prepare(44100.0, N);
    rt.setQuirkProfile(engine::fx::kQuirkProfileReference);
    st.setQuirkProfile(engine::fx::kQuirkProfileSixteen);
    for (auto* e : {&rt, &st})
    {
        e->setParam(0, 0.5f);
        e->reset();
        run(*e, constant(2, 0.0f));
    }
    CHECK(rt.band(0).fc == 1000.0f);
    CHECK(st.band(0).fc == ParametricEq::freqOf(0.5f, false));
}

TEST_CASE("fx.eq2: reset zeroes only the current stages' taps (FX-460); kClearState = fresh", "[fx][eq2]")
{
    ParametricEq e, fresh;
    e.prepare(48000.0, N);
    fresh.prepare(48000.0, N);
    defaults(e);
    e.setParam(1, 1.0f);
    e.reset();
    const auto in = noise(10, 9);
    run(e, in);
    e.setOption(engine::fx::kClearState, 1);
    for (auto* x : {&e, &fresh})
    {
        defaults(*x);
        x->reset();
    }
    const auto a = run(e, in), b = run(fresh, in);
    CHECK(sameBits(a.l, b.l));
    CHECK(sameBits(a.r, b.r));
}

TEST_CASE("fx.eq2: neon_vs_scalar_eq2 — the NEON path equals the scalar reference bit for bit", "[fx][eq2][simd]")
{
#if ! APP_FX_HAVE_NEON
    SKIP("no NEON on this host (scripts/neon-tests.sh runs this under qemu-arm)");
#else
    core::rt::enableFlushToZero(); // the app's audio-thread numerics (FX-055): VFP flushes like NEON
    for (std::uint32_t profile : {engine::fx::kQuirkProfileReference, engine::fx::kQuirkProfileSixteen})
        for (std::uint32_t seed : {3u, 99u, 31337u})
        {
            ParametricEq a, b;
            for (auto* e : {&a, &b})
            {
                e->prepare(44100.0, N);
                e->setQuirkProfile(profile);
                defaults(*e);
                e->reset();
            }
            auto oa = noise(200, seed * 5 + 2, 0.8f), ob = oa;
            std::uint32_t x = seed;
            auto draw = [&]
            {
                x ^= x << 13;
                x ^= x >> 17;
                x ^= x << 5;
                return x;
            };
            for (int blk = 0; blk < 200; ++blk)
            {
                if (draw() % 3 == 0)
                {
                    const std::uint32_t w = draw();
                    const float v = static_cast<float>(w >> 8) * (1.0f / 16777216.0f);
                    const int what = static_cast<int>(w % 8);
                    for (auto* e : {&a, &b})
                    {
                        if (what < 6)
                            e->setParam(what, v);
                        else
                            e->setOption(what - 6, static_cast<int>(w >> 3) % 5);
                    }
                }
                block(a, oa.l.data() + blk * N, oa.r.data() + blk * N, false);
                block(b, ob.l.data() + blk * N, ob.r.data() + blk * N, true);
            }
            CHECK(sameBits(oa.l, ob.l));
            CHECK(sameBits(oa.r, ob.r));
        }
#endif
}
