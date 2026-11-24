// Reverb behaviour (engine/fx/effects/Reverb.h, the 8-line FDN): measured T60 against the decay curve, the damping's
// high-frequency loss, the absorbent filters' exact gains, the stereo image, the first output sample, no dry at Send
// Mix 1, reset and kClearState, the sleep of a silent reverb and its wake-up, the room glide, stability at the
// extremes, the laws and the parameter texts (all four slot kinds), and memory per instance. Scalar ↔ NEON parity:
// test_fx_dly_neon.cpp (neon_vs_scalar_reverb and the kernels), run under qemu by scripts/neon-tests.sh.
#include "tests/unit/fx/FxTestUtil.h"

#include "core/util/Realtime.h"
#include "engine/fx/FxShell.h"
#include "engine/fx/dsp/Filters.h"
#include "engine/fx/effects/Reverb.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

using namespace engine::fx;

namespace
{
constexpr int N = engine::kBlockFrames;

std::unique_ptr<Reverb> make(double fs, const float (&k)[6])
{
    auto r = std::make_unique<Reverb>();
    r->prepare(fs, N);
    for (int i = 0; i < 6; ++i)
        r->setParam(i, k[i]);
    r->reset();
    return r;
}

struct Out
{
    std::vector<float> l, r;
};
// Runs the effect alone (wet only) over the input.
Out run(Reverb& e, const std::vector<float>& inL, const std::vector<float>& inR)
{
    Out o;
    o.l.resize(inL.size());
    o.r.resize(inR.size());
    alignas(16) float l[N], r[N];
    const ProcessContext ctx = fxtest::context();
    for (std::size_t b = 0; b + N <= inL.size(); b += N)
    {
        std::memcpy(l, inL.data() + b, sizeof l);
        std::memcpy(r, inR.data() + b, sizeof r);
        e.process(l, r, ctx);
        std::memcpy(o.l.data() + b, l, sizeof l);
        std::memcpy(o.r.data() + b, r, sizeof r);
    }
    return o;
}

std::vector<float> impulse(std::size_t n, std::size_t at, float a = 1.0f)
{
    std::vector<float> v(n, 0.0f);
    v[at] = a;
    return v;
}

std::vector<float> noise(std::size_t n, unsigned seed, std::size_t until = ~std::size_t{0})
{
    std::vector<float> v(n, 0.0f);
    unsigned s = seed;
    for (std::size_t i = 0; i < n && i < until; ++i)
    {
        s = s * 1664525u + 1013904223u;
        v[i] = 0.5f * static_cast<float>(static_cast<int>(s >> 8) - (1 << 23)) / static_cast<float>(1 << 23);
    }
    return v;
}

int firstNonZero(const std::vector<float>& v)
{
    for (std::size_t i = 0; i < v.size(); ++i)
        if (v[i] != 0.0f)
            return static_cast<int>(i);
    return -1;
}

double energy(const std::vector<float>& v, std::size_t from, std::size_t to)
{
    double e = 0.0;
    for (std::size_t i = from; i < to && i < v.size(); ++i)
        e += static_cast<double>(v[i]) * static_cast<double>(v[i]);
    return e;
}

// T60 from the band-limited impulse response: the energy envelope in 50 ms windows (dB), a least-squares line over
// the windows from 150 ms to `fitSeconds`, T60 = 60 dB / slope.
double measureT60(const std::vector<float>& ir, double fs, double f0, double f1, double fitSeconds)
{
    std::vector<float> l(ir), r(ir);
    dsp::Biquad2 hp, lp;
    hp.setCoefs(dsp::BiquadCoefs::design(dsp::BiquadCoefs::Type::HighPass, fs, f0, 0.7071));
    lp.setCoefs(dsp::BiquadCoefs::design(dsp::BiquadCoefs::Type::LowPass, fs, f1, 0.7071));
    for (std::size_t b = 0; b + N <= l.size(); b += N)
    {
        hp.processScalar(l.data() + b, r.data() + b, N);
        lp.processScalar(l.data() + b, r.data() + b, N);
    }
    const auto win = static_cast<std::size_t>(0.05 * fs);
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int n = 0;
    for (std::size_t s = static_cast<std::size_t>(0.15 * fs); s + win <= l.size() && s < fitSeconds * fs; s += win)
    {
        const double e = energy(l, s, s + win) + 1e-300;
        const double t = static_cast<double>(s) / fs, y = 10.0 * std::log10(e);
        sx += t;
        sy += y;
        sxx += t * t;
        sxy += t * y;
        ++n;
    }
    const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
    return -60.0 / slope;
}

double energyAbove(const std::vector<float>& v, std::size_t from, double fs, double fc)
{
    std::vector<float> l(v.begin() + static_cast<std::ptrdiff_t>(from), v.end()), r(l);
    dsp::Biquad2 hp1, hp2;
    const auto c = dsp::BiquadCoefs::design(dsp::BiquadCoefs::Type::HighPass, fs, fc, 0.7071);
    hp1.setCoefs(c);
    hp2.setCoefs(c);
    for (std::size_t b = 0; b + N <= l.size(); b += N)
    {
        hp1.processScalar(l.data() + b, r.data() + b, N);
        hp2.processScalar(l.data() + b, r.data() + b, N);
    }
    return energy(l, 0, l.size());
}
} // namespace

TEST_CASE("fx.reverb: measured T60 follows the decay curve (room 50, damping 0)", "[fx][reverb][behaviour]")
{
    const double fs = 44100.0;
    struct Row
    {
        float v;
        double seconds, fit;
    };
    for (const Row row :
         {Row{0.09f, 2.0, 1.2}, Row{0.3f, 5.0, 3.5}, Row{49.0f / 99.0f, 10.0, 8.0}, Row{1.0f, 40.0, 38.0}})
    {
        const float knobs[6] = {0.0f, 49.0f / 99.0f, row.v, 0.0f, 0.0f, 1.0f};
        auto e = make(fs, knobs);
        const auto n = static_cast<std::size_t>(row.seconds * fs) / N * N;
        // a loud impulse: the tail must stay above the sleep level (−120 dBFS) over the whole fit
        const auto o = run(*e, impulse(n, 64, 1000.0f), std::vector<float>(n, 0.0f));
        const double target = Reverb::t60(row.v);
        const double tMid = measureT60(o.l, fs, 500.0, 2000.0, row.fit);
        const double tLow = measureT60(o.l, fs, 125.0, 500.0, row.fit);
        std::printf("fx.reverb: T60 v=%.4f curve %.3f s, measured 500 Hz-2 kHz %.3f s (%+.1f %%), 125-500 Hz %.3f s "
                    "(%+.1f %%)\n",
                    static_cast<double>(row.v), target, tMid, 100.0 * (tMid / target - 1.0), tLow,
                    100.0 * (tLow / target - 1.0));
        INFO("v " << row.v << " target " << target << " measured " << tMid << " low band " << tLow);
        CHECK(std::fabs(tLow / target - 1.0) <= 0.10);
        // the modulated lines' linear reads lose a little above 500 Hz on every pass, which shows only when the DC
        // loss per pass is tiny (a 100 s decay)
        if (row.v < 1.0f)
            CHECK(std::fabs(tMid / target - 1.0) <= 0.15);
        else
            CHECK(tMid / target >= 0.6);
    }
}

TEST_CASE("fx.reverb: Damping 1 has >= 12 dB less energy above 8 kHz than Damping 0 after 1 s",
          "[fx][reverb][behaviour]")
{
    const double fs = 44100.0;
    const std::size_t n = static_cast<std::size_t>(2.0 * fs) / N * N;
    double e[2]{};
    for (int i = 0; i < 2; ++i)
    {
        const float knobs[6] = {0.0f, 49.0f / 99.0f, 49.0f / 99.0f, i == 0 ? 0.0f : 1.0f, 0.0f, 1.0f};
        auto r = make(fs, knobs);
        const auto o = run(*r, impulse(n, 64, 1000.0f), impulse(n, 64, 1000.0f));
        e[i] = energyAbove(o.l, static_cast<std::size_t>(fs), fs, 8000.0);
    }
    const double db = 10.0 * std::log10(e[0] / e[1]);
    std::printf("fx.reverb: energy above 8 kHz after 1 s, damping 0 vs 1: %.1f dB\n", db);
    CHECK(db >= 12.0);
}

TEST_CASE("fx.reverb: the absorbent filter: gain g at DC, g^3 at the damping frequency, at most −3 dB per pass there",
          "[fx][reverb]")
{
    for (double d : {300.0, 1301.0, 2907.0})
        for (double t : {0.1, 1.0, 8.7, 100.6})
            for (double fd : {2000.0, 9000.0, 18000.0})
            {
                float a = 0.0f, b = 0.0f;
                dsp::reverb::absorbent(d, 44100.0, t, fd, 3.0, a, b);
                const double g = std::pow(10.0, -3.0 * d / (44100.0 * t));
                const auto H = [&](double f)
                {
                    const std::complex<double> z = std::polar(1.0, -2.0 * 3.141592653589793 * f / 44100.0);
                    return std::abs(static_cast<double>(b) / (1.0 - static_cast<double>(a) * z));
                };
                INFO("D " << d << " T " << t << " fd " << fd << " a " << a << " b " << b);
                CHECK(a >= 0.0f);
                CHECK(a < 1.0f);
                CHECK(H(0.0) == Catch::Approx(g).epsilon(1e-5));
                const double want = std::max(g * g * g, g * std::sqrt(0.5)); // T60/3 at fd, capped at −3 dB per pass
                CHECK(H(fd) == Catch::Approx(want).epsilon(1e-4));
                CHECK(H(22050.0) <= H(fd) * (1.0 + 1e-6));
            }
}

TEST_CASE("fx.reverb: stereo — the left input answers on the left first, the tail is decorrelated",
          "[fx][reverb][behaviour]")
{
    const double fs = 44100.0;
    const float knobs[6] = {0.0f, 49.0f / 99.0f, 0.4f, 0.2f, 0.0f, 1.0f};
    auto e = make(fs, knobs);
    const std::size_t n = static_cast<std::size_t>(1.5 * fs) / N * N;
    const auto o = run(*e, impulse(n, 100), std::vector<float>(n, 0.0f));
    const auto early = static_cast<std::size_t>(0.04 * fs);
    const double eL = energy(o.l, 0, early), eR = energy(o.r, 0, early);
    std::printf("fx.reverb: L-only impulse, first 40 ms: L %.1f dB above R\n", 10.0 * std::log10(eL / (eR + 1e-30)));
    CHECK(eL > 10.0 * eR);
    // a stereo noise burst: the late field's L/R correlation stays small
    auto e2 = make(fs, knobs);
    const auto o2 =
        run(*e2, noise(n, 3u, static_cast<std::size_t>(0.2 * fs)), noise(n, 9u, static_cast<std::size_t>(0.2 * fs)));
    double lr = 0.0;
    const auto from = static_cast<std::size_t>(0.4 * fs);
    for (std::size_t i = from; i < n; ++i)
        lr += static_cast<double>(o2.l[i]) * static_cast<double>(o2.r[i]);
    const double corr = lr / std::sqrt(energy(o2.l, from, n) * energy(o2.r, from, n));
    std::printf("fx.reverb: late L/R correlation %.3f\n", corr);
    CHECK(std::fabs(corr) < 0.3);
}

TEST_CASE("fx.reverb: Spread widens the image (side/mid energy grows)", "[fx][reverb][behaviour]")
{
    const double fs = 44100.0;
    const std::size_t n = static_cast<std::size_t>(1.0 * fs) / N * N;
    double ratio[2]{};
    for (int i = 0; i < 2; ++i)
    {
        const float knobs[6] = {0.0f, 0.5f, 0.4f, 0.2f, i == 0 ? 0.0f : 1.0f, 1.0f};
        auto e = make(fs, knobs);
        const auto in = noise(n, 5u, static_cast<std::size_t>(0.1 * fs));
        const auto o = run(*e, in, in);
        double m = 0.0, s = 0.0;
        for (std::size_t k = 0; k < n; ++k)
        {
            const double a = static_cast<double>(o.l[k]), b = static_cast<double>(o.r[k]);
            m += (a + b) * (a + b);
            s += (a - b) * (a - b);
        }
        ratio[i] = s / m;
    }
    CHECK(ratio[1] == Catch::Approx(4.0 * ratio[0]).epsilon(0.01)); // width 2: the side doubled
}

TEST_CASE("fx.reverb: the first output sample is at the pre-delay (the diffused early path)", "[fx][reverb][behaviour]")
{
    const double fs = 44100.0;
    for (float pd : {0.0f, 0.25f, 0.5f, 1.0f})
        for (float room : {0.0f, 49.0f / 99.0f, 1.0f})
        {
            const float knobs[6] = {pd, room, 0.5f, 0.0f, 0.0f, 1.0f};
            auto e = make(fs, knobs);
            const std::size_t at = 1000, n = 64 * 160;
            const auto o = run(*e, impulse(n, at), impulse(n, at));
            const double pdS = std::round(std::max(0.1, 100.0 * static_cast<double>(pd)) * fs / 1000.0);
            const double expected = static_cast<double>(at) + pdS;
            INFO("pre-delay " << pd << " room " << room << " expected " << expected);
            CHECK(std::fabs(firstNonZero(o.l) - expected) <= 1.0);
            CHECK(std::fabs(firstNonZero(o.r) - expected) <= 1.0);
        }
}

TEST_CASE("fx.reverb: Send Mix 1 has no dry component (FX-093/828)", "[fx][reverb][behaviour]")
{
    const auto* d = EffectRegistry::builtin().find("reverb");
    REQUIRE(d != nullptr);
    auto e = d->create();
    e->prepare(44100.0, N);
    FxShell shell;
    alignas(16) float cl[N], cr[N];
    shell.prepare(*e, d->shell, 44100.0, SlotKind::Send, cl, cr);
    const float knobs[6] = {0.5f, 0.5f, 0.5f, 0.0f, 0.0f, 1.0f};
    for (int k = 0; k < 6; ++k)
        shell.setParam(k, knobs[k]);
    shell.activate(false);
    CHECK(shell.dry() == 0.0f);
    const ProcessContext ctx = fxtest::context();
    alignas(16) float l[N], r[N];
    for (int b = 0; b < 40; ++b) // after the activation fade (147 samples), a dry impulse never reaches the output
    {
        std::memset(l, 0, sizeof l);
        std::memset(r, 0, sizeof r);
        if (b == 10)
            l[5] = r[5] = 1.0f;
        shell.process(l, r, ctx);
        if (b == 10)
        {
            CHECK(l[5] == 0.0f);
            CHECK(r[5] == 0.0f);
        }
    }
}

TEST_CASE("fx.reverb: reset and kClearState silence the rings", "[fx][reverb][behaviour]")
{
    const double fs = 44100.0;
    const float knobs[6] = {0.75f, 0.5f, 0.6f, 0.0f, 0.0f, 1.0f};
    const std::size_t n = 64 * 100;
    for (int how = 0; how < 2; ++how)
    {
        auto e = make(fs, knobs);
        (void)run(*e, noise(n, 7u), noise(n, 8u));
        if (how == 0)
            e->reset();
        else
            e->setOption(kClearState, 1);
        const auto o = run(*e, std::vector<float>(n, 0.0f), std::vector<float>(n, 0.0f));
        INFO((how == 0 ? "reset" : "kClearState"));
        CHECK(firstNonZero(o.l) < 0);
        CHECK(firstNonZero(o.r) < 0);
    }
}

TEST_CASE("fx.reverb: a silent reverb sleeps once its tail is gone and wakes on the next input", "[fx][reverb]")
{
    const double fs = 44100.0;
    const float knobs[6] = {0.2f, 0.5f, 0.3f, 0.3f, 0.0f, 1.0f}; // T60 3.55 s
    auto e = make(fs, knobs);
    CHECK_FALSE(e->sleeping());
    const auto burst = static_cast<std::size_t>(0.1 * fs);
    const std::size_t n = static_cast<std::size_t>(12.0 * fs) / N * N;
    const auto o = run(*e, noise(n, 11u, burst), noise(n, 12u, burst));
    REQUIRE(e->sleeping());
    // it fell asleep only below −110 dBFS: the last non-zero sample is small, everything after it exactly 0
    int last = -1;
    for (std::size_t i = 0; i < n; ++i)
        if (o.l[i] != 0.0f || o.r[i] != 0.0f)
            last = static_cast<int>(i);
    REQUIRE(last > static_cast<int>(burst));
    const double tailSeconds = static_cast<double>(last) / fs;
    std::printf("fx.reverb: T60 3.55 s, 0.1 s burst: asleep after %.2f s\n", tailSeconds);
    CHECK(tailSeconds > 3.0);
    CHECK(tailSeconds < 11.0);
    float peakEnd = 0.0f;
    for (int i = last - N; i <= last; ++i)
        peakEnd = std::max(peakEnd, std::max(std::fabs(o.l[static_cast<std::size_t>(i)]),
                                             std::fabs(o.r[static_cast<std::size_t>(i)])));
    CHECK(peakEnd < 1.0e-4f);
    // asleep: silence in, silence out, no work
    const auto z = run(*e, std::vector<float>(64 * 50, 0.0f), std::vector<float>(64 * 50, 0.0f));
    CHECK(firstNonZero(z.l) < 0);
    CHECK(e->sleeping());
    // an impulse wakes it at once: the early path answers after the pre-delay of this block
    const auto w = run(*e, impulse(64 * 50, 3), impulse(64 * 50, 3));
    CHECK_FALSE(e->sleeping());
    const double pdS = std::round(20.0 * fs / 1000.0);
    CHECK(firstNonZero(w.l) == static_cast<int>(3.0 + pdS));
}

TEST_CASE("fx.reverb: room glide — every line moves smoothly to its new length", "[fx][reverb]")
{
    const double fs = 44100.0;
    const float knobs[6] = {0.0f, 0.0f, 0.5f, 0.0f, 0.0f, 1.0f};
    auto e = make(fs, knobs);
    CHECK(e->room() == 1);
    for (int i = 0; i < Reverb::kLines; ++i) // the reset snaps to the stored room
        CHECK(e->lineDelay(i) == e->lineTarget(i));
    float before[Reverb::kLines];
    for (int i = 0; i < Reverb::kLines; ++i)
        before[i] = e->lineDelay(i);
    e->setParam(1, 1.0f);
    alignas(16) float l[N]{}, r[N]{};
    float far = 0.0f;
    for (int b = 0; b < 10; ++b)
    {
        float prev[Reverb::kLines];
        for (int i = 0; i < Reverb::kLines; ++i)
            prev[i] = e->lineDelay(i);
        std::memset(l, 0, sizeof l);
        std::memset(r, 0, sizeof r);
        l[0] = r[0] = 0.1f;
        e->process(l, r, fxtest::context());
        for (int i = 0; i < Reverb::kLines; ++i)
            far = std::max(far, std::fabs(e->lineDelay(i) - prev[i]));
    }
    CHECK(e->room() == 100);
    CHECK(far <= 0.03f * static_cast<float>(N) + 1e-3f); // at most 3 % pitch while gliding
    for (int i = 0; i < Reverb::kLines; ++i)
    {
        CHECK(e->lineDelay(i) > before[i]);
        CHECK(e->lineDelay(i) < e->lineTarget(i));
        // the line lengths at room 100: the base lengths, odd sample counts
        CHECK(std::fabs(e->lineTarget(i) - Reverb::baseDelayMs(i) * 44.1f) <= 1.0f);
        CHECK(std::fmod(e->lineTarget(i), 2.0f) == 1.0f);
    }
    for (int b = 0; b < 2000; ++b)
    {
        std::memset(l, 0, sizeof l);
        std::memset(r, 0, sizeof r);
        l[0] = r[0] = 0.1f;
        e->process(l, r, fxtest::context());
    }
    for (int i = 0; i < Reverb::kLines; ++i)
        CHECK(e->lineDelay(i) == e->lineTarget(i));
}

TEST_CASE("fx.reverb: stable and bounded at the extremes", "[fx][reverb][behaviour]")
{
    const double fs = 44100.0;
    for (float damping : {0.0f, 1.0f})
    {
        const float knobs[6] = {0.0f, 1.0f, 1.0f, damping, 1.0f, 1.0f}; // room 100, T60 100.6 s, widest
        auto e = make(fs, knobs);
        const std::size_t n = static_cast<std::size_t>(8.0 * fs) / N * N;
        auto inL = noise(n, 21u), inR = noise(n, 22u);
        alignas(16) float l[N], r[N];
        float peak = 0.0f;
        bool finite = true;
        for (std::size_t b = 0; b + N <= n; b += N)
        {
            std::memcpy(l, inL.data() + b, sizeof l);
            std::memcpy(r, inR.data() + b, sizeof r);
            if (b % (N * 300) == 0) // the room and the time keep moving
            {
                e->setParam(1, (b / (N * 300)) % 2 == 0 ? 0.0f : 1.0f);
                e->setParam(2, (b / (N * 300)) % 3 == 0 ? 0.6f : 1.0f);
            }
            e->process(l, r, fxtest::context());
            for (int i = 0; i < N; ++i)
            {
                finite = finite && std::isfinite(l[i]) && std::isfinite(r[i]);
                peak = std::max(peak, std::max(std::fabs(l[i]), std::fabs(r[i])));
            }
        }
        std::printf("fx.reverb: 8 s of noise at T60 100 s, damping %.0f: peak %.2f\n", static_cast<double>(damping),
                    static_cast<double>(peak));
        CHECK(finite);
        CHECK(peak < 8.0f);
    }
}

TEST_CASE("fx.reverb: laws — decay anchors, room scale, input gain, damping", "[fx][reverb]")
{
    const float v[5] = {0.0f, 0.09f, 0.3f, 49.0f / 99.0f, 1.0f};
    const float s[5] = {0.10f, 0.87f, 3.55f, 8.70f, 100.6f};
    for (int i = 0; i < 5; ++i)
        CHECK(Reverb::t60(v[i]) == Catch::Approx(s[i]).epsilon(1e-6));
    float last = 0.0f;
    for (int i = 0; i <= 1000; ++i) // monotonic
    {
        const float t = Reverb::t60(static_cast<float>(i) / 1000.0f);
        CHECK(t > last);
        last = t;
    }
    CHECK(Reverb::roomScale(1) == Catch::Approx(0.2f));
    CHECK(Reverb::roomScale(100) == Catch::Approx(1.0f));
    CHECK(Reverb::roomScale(50) > Reverb::roomScale(49));
    CHECK(Reverb::inputGain(0.0f) == Catch::Approx(0.2f));
    CHECK(Reverb::inputGain(0.5137255f) == Catch::Approx(0.2f));
    CHECK(Reverb::inputGain(1.0f) == Catch::Approx(0.1f).epsilon(1e-5));
    CHECK(Reverb::dampingMaxHz(44100.0) == 18000.0f);
    CHECK(Reverb::dampingMaxHz(32000.0) == 12800.0f);
    CHECK(Reverb::dampingMaxHz(96000.0) == 18000.0f);
    CHECK(Reverb::dampingHz(0.0f, 44100.0) == 18000.0f);
    CHECK(Reverb::dampingHz(1.0f, 44100.0) == Catch::Approx(2000.0f));
    // the shortest line at room 1 stays longer than a block plus the modulation (the block-wise network needs it)
    for (int i = 0; i < Reverb::kLines; ++i)
        CHECK(Reverb::baseDelayMs(i) * Reverb::roomScale(1) * 44.1f > static_cast<float>(N) + 0.25f * 44.1f + 4.0f);
}

TEST_CASE("fx.reverb: parameter texts follow the engine laws; every slot kind has the reverb", "[fx][reverb]")
{
    const auto& reg = core::builtinParams();
    const auto* time = reg.find("send.reverb.time");
    const auto* room = reg.find("master.reverb.room");
    const auto* mix = reg.find("send.reverb.mix");
    const auto* damping = reg.find("send.reverb.damping");
    REQUIRE(time != nullptr);
    REQUIRE(room != nullptr);
    REQUIRE(mix != nullptr);
    REQUIRE(damping != nullptr);
    CHECK(! time->lockable); // FX-094
    CHECK(mix->defaultValue == 1.0f);
    CHECK(reg.find("master.reverb.mix")->defaultValue == 0.5f);
    for (const char* id : {"fx1.reverb.mix", "fx2.reverb.mix", "fx1.reverb.time", "fx2.reverb.preDelay"})
    {
        INFO(id);
        REQUIRE(reg.find(id) != nullptr);
    }
    CHECK(reg.find("fx1.reverb.mix")->defaultValue == 0.3f);
    char buf[32];
    room->format(49.0f / 99.0f, buf, sizeof buf);
    CHECK(std::string(buf) == "50");
    for (float x : {0.0f, 0.09f, 0.21f, 0.3f, 49.0f / 99.0f, 0.77f, 1.0f})
    {
        time->format(x, buf, sizeof buf);
        char want[32];
        const double t = Reverb::t60(x);
        std::snprintf(want, sizeof want, t < 10.0 ? "%.2f s" : "%.1f s", t);
        CHECK(std::string(buf) == want);
    }
    for (float x : {0.0f, 0.3f, 0.5f, 1.0f})
    {
        damping->format(x, buf, sizeof buf);
        char want[32];
        std::snprintf(want, sizeof want, "%.1f kHz", static_cast<double>(Reverb::dampingHz(x, 44100.0)) / 1000.0);
        CHECK(std::string(buf) == want);
    }
    const auto* d = EffectRegistry::builtin().find("reverb");
    REQUIRE(d != nullptr);
    CHECK(allows(d->slots, SlotKind::Insert));
    CHECK(allows(d->slots, SlotKind::Send));
    CHECK(allows(d->slots, SlotKind::Master));
}

TEST_CASE("fx.reverb: memory per instance", "[fx][reverb][memory]")
{
    for (double fs : {44100.0, 48000.0, 96000.0})
    {
        Reverb r;
        r.prepare(fs, N);
        std::printf("fx.reverb: %.0f Hz: %zu bytes per instance, 34 slots %.2f MiB\n", fs, r.bytes(),
                    34.0 * static_cast<double>(r.bytes()) / 1048576.0);
        CHECK(r.bytes() < (fs <= 48000.0 ? 140000u : 280000u));
    }
}
