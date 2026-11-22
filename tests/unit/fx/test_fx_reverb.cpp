// Reverb behaviour (spec 02-features/effects/reverb.md "Test notes" level 1, always run): the app's own
// constants (FX-821/822/827) make sample goldens against the reference impossible, so these check the laws instead —
// measured T60 against the FX-822 curve, L-only input at Spread 0 → R exactly 0, the first output sample, no dry at
// Send Mix 1, the damping's high-frequency loss, the Sixteen fixes (FX-880/881/884/886) against the Reference profile,
// and the parameter laws and texts. The structural golden is tests/golden/reverb (the model with the app's constants).
#include "tests/unit/fx/FxTestUtil.h"

#include "core/util/Realtime.h"
#include "engine/fx/FxShell.h"
#include "engine/fx/dsp/Filters.h"
#include "engine/fx/effects/Reverb.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

using namespace engine::fx;

namespace
{
constexpr int N = engine::kBlockFrames;

std::unique_ptr<Reverb> make(double fs, const float (&k)[6], std::uint32_t quirks = kQuirkProfileSixteen)
{
    auto r = std::make_unique<Reverb>();
    r->prepare(fs, N);
    r->setQuirkProfile(quirks);
    for (int i = 0; i < 6; ++i)
        r->setParam(i, k[i]);
    r->reset();
    return r;
}

// Runs the effect alone (wet only) over the input; returns the interleaved-per-channel output.
struct Out
{
    std::vector<float> l, r;
};
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

int firstNonZero(const std::vector<float>& v)
{
    for (std::size_t i = 0; i < v.size(); ++i)
        if (v[i] != 0.0f)
            return static_cast<int>(i);
    return -1;
}

// T60 from the band-limited impulse response: the energy envelope in 50 ms windows (dB), a least-squares line over
// the windows from 150 ms to `fitSeconds`, T60 = 60 dB / slope. A direct fit has no Schroeder truncation bias.
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
        double e = 1e-300;
        for (std::size_t i = s; i < s + win; ++i)
            e += static_cast<double>(l[i]) * static_cast<double>(l[i]);
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
    double e = 0;
    for (std::size_t b = 0; b + N <= l.size(); b += N)
    {
        hp1.processScalar(l.data() + b, r.data() + b, N);
        hp2.processScalar(l.data() + b, r.data() + b, N);
        for (int i = 0; i < N; ++i)
        {
            const auto x = static_cast<double>(l[b + static_cast<std::size_t>(i)]);
            e += x * x;
        }
    }
    return e;
}
} // namespace

TEST_CASE("fx.reverb: measured T60 follows the FX-822 curve (room 50, damping 0)", "[fx][reverb][behaviour]")
{
    const double fs = 44100.0;
    const ReverbConstants k;
    // v, the record length (s), the fit range end (s), the band
    struct Row
    {
        float v;
        double seconds, fit;
    };
    for (const Row row : {Row{0.09f, 2.0, 1.2}, Row{0.3f, 5.0, 3.5}, Row{49.0f / 99.0f, 10.0, 8.0}, Row{1.0f, 40.0, 38.0}})
    {
        const float knobs[6] = {0.0f, 49.0f / 99.0f, row.v, 0.0f, 0.0f, 1.0f};
        auto e = make(fs, knobs);
        const auto n = static_cast<std::size_t>(row.seconds * fs) / N * N;
        const auto in = impulse(n, 64);
        const auto zero = std::vector<float>(n, 0.0f);
        const auto o = run(*e, in, zero);
        const double target = Reverb::t60(k, row.v);
        const double t500 = measureT60(o.l, fs, 500.0, 4000.0, row.fit);
        const double tLow = measureT60(o.l, fs, 125.0, 500.0, row.fit);
        std::printf("fx.reverb: T60 v=%.4f curve %.3f s, measured 500 Hz-4 kHz %.3f s (%+.1f %%), 125-500 Hz %.3f s "
                    "(%+.1f %%)\n",
                    static_cast<double>(row.v), target, t500, 100.0 * (t500 / target - 1.0), tLow,
                    100.0 * (tLow / target - 1.0));
        INFO("v " << row.v << " target " << target << " measured " << t500 << " low band " << tLow);
        CHECK(std::fabs(tLow / target - 1.0) <= 0.10);
        if (row.v < 1.0f)
            CHECK(std::fabs(t500 / target - 1.0) <= 0.10);
        else // the in-loop damping LP (18 kHz at Damping 0, FX-824) shortens a 100 s decay above 500 Hz (ours)
            CHECK(t500 / target >= 0.70);
    }
}

TEST_CASE("fx.reverb: L-only input at Spread 0 gives R = 0 exactly (FX-826)", "[fx][reverb][behaviour]")
{
    for (float room : {0.0f, 0.5f, 1.0f})
    {
        const float knobs[6] = {0.3f, room, 0.7f, 0.5f, 0.0f, 1.0f};
        auto e = make(44100.0, knobs);
        const std::size_t n = 64 * N * 40;
        std::vector<float> in(n);
        unsigned s = 1u;
        for (auto& x : in)
        {
            s = s * 1664525u + 1013904223u;
            x = static_cast<float>(static_cast<int>(s >> 8) - (1 << 23)) / static_cast<float>(1 << 23);
        }
        const auto o = run(*e, in, std::vector<float>(n, 0.0f));
        bool rZero = true, lAny = false;
        for (std::size_t i = 0; i < n; ++i)
        {
            rZero = rZero && o.r[i] == 0.0f;
            lAny = lAny || o.l[i] != 0.0f;
        }
        CHECK(rZero);
        CHECK(lAny);
    }
}

TEST_CASE("fx.reverb: the first output sample is at pre-delay + shortest comb (±1)", "[fx][reverb][behaviour]")
{
    // The all-passes' feed-forward path (y = ring[w − D] − g·x) passes the first comb arrival at once, so the
    // all-pass delays do not postpone the first sample (the spec's "+ allpass delays" counts them; ours, reported).
    // The pre-delay writes first and reads at (w + 1) − D (FX-820): its first non-zero sample is at D − 1.
    const double fs = 44100.0;
    const ReverbConstants k;
    for (float pd : {0.0f, 0.25f, 0.5f, 1.0f})
        for (float room : {0.0f, 49.0f / 99.0f, 1.0f})
        {
            const float knobs[6] = {pd, room, 0.5f, 0.0f, 0.0f, 1.0f};
            auto e = make(fs, knobs);
            const std::size_t at = 1000, n = 64 * 160;
            const auto o = run(*e, impulse(n, at), impulse(n, at));
            const int roomI = static_cast<int>(1.0f + 99.0f * room);
            const double s = Reverb::roomScale(roomI);
            double shortestL = 1e9, shortestR = 1e9;
            for (int c = 0; c < 5; ++c)
            {
                shortestL = std::min(shortestL, s * static_cast<double>(k.combL[static_cast<std::size_t>(c)]));
                shortestR = std::min(shortestR, s * static_cast<double>(k.combR[static_cast<std::size_t>(c)]));
            }
            const double pdS = std::max(0.1, 100.0 * static_cast<double>(pd)) * fs / 1000.0;
            const double expL = static_cast<double>(at) + (pdS - 1.0) + shortestL * fs / 1000.0;
            const double expR = static_cast<double>(at) + (pdS - 1.0) + shortestR * fs / 1000.0;
            INFO("pre-delay " << pd << " room " << roomI << " expected L " << expL << " R " << expR);
            CHECK(std::fabs(firstNonZero(o.l) - expL) <= 1.0);
            CHECK(std::fabs(firstNonZero(o.r) - expR) <= 1.0);
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

TEST_CASE("fx.reverb: Damping 1 has >= 12 dB less energy above 8 kHz than Damping 0 after 1 s", "[fx][reverb][behaviour]")
{
    const double fs = 44100.0;
    const std::size_t n = static_cast<std::size_t>(2.0 * fs) / N * N;
    double e[2]{};
    for (int i = 0; i < 2; ++i)
    {
        const float knobs[6] = {0.0f, 49.0f / 99.0f, 49.0f / 99.0f, i == 0 ? 0.0f : 1.0f, 0.0f, 1.0f};
        auto r = make(fs, knobs);
        const auto o = run(*r, impulse(n, 64), impulse(n, 64));
        e[i] = energyAbove(o.l, static_cast<std::size_t>(fs), fs, 8000.0);
    }
    const double db = 10.0 * std::log10(e[0] / e[1]);
    std::printf("fx.reverb: energy above 8 kHz after 1 s, damping 0 vs 1: %.1f dB\n", db);
    CHECK(db >= 12.0);
}

TEST_CASE("fx.reverb: Sixteen fixes vs the Reference profile (FX-880/881/886)", "[fx][reverb][behaviour]")
{
    const double fs = 44100.0;
    const float knobs[6] = {0.75f, 0.5f, 0.6f, 0.0f, 0.0f, 1.0f};
    SECTION("FX-880: no warm-up silence")
    {
        // an impulse at sample 0: the Sixteen reverb answers after pre-delay + comb; the Reference one stays muted
        // until its rings have filled once (≈ 101 ms)
        auto six = make(fs, knobs, kQuirkProfileSixteen);
        auto ref = make(fs, knobs, kQuirkProfileReference);
        const std::size_t n = 64 * 120;
        const auto a = run(*six, impulse(n, 0), impulse(n, 0));
        const auto b = run(*ref, impulse(n, 0), impulse(n, 0));
        CHECK(firstNonZero(a.l) > 0);
        CHECK(firstNonZero(a.l) < 4454);
        CHECK((firstNonZero(b.l) < 0 || firstNonZero(b.l) >= 4454));
    }
    SECTION("FX-881: the reset zeroes the rings")
    {
        for (auto q : {kQuirkProfileSixteen, kQuirkProfileReference})
        {
            auto e = make(fs, knobs, q);
            const std::size_t n = 64 * 100;
            std::vector<float> noise(n);
            unsigned s = 7u;
            for (auto& x : noise)
            {
                s = s * 1664525u + 1013904223u;
                x = static_cast<float>(static_cast<int>(s >> 8) - (1 << 23)) / static_cast<float>(1 << 23);
            }
            (void) run(*e, noise, noise);
            e->reset();
            const auto o = run(*e, std::vector<float>(n, 0.0f), std::vector<float>(n, 0.0f));
            const bool silent = firstNonZero(o.l) < 0 && firstNonZero(o.r) < 0;
            INFO("profile " << q);
            CHECK(silent == (q == kQuirkProfileSixteen));
        }
    }
    SECTION("FX-886: the comb glide time is the current t after a reset")
    {
        auto six = make(fs, knobs, kQuirkProfileSixteen);
        auto ref = make(fs, knobs, kQuirkProfileReference);
        CHECK(six->comb(0).glide() == 40.0f);
        CHECK(ref->comb(0).glide() == 100.0f);
    }
}

TEST_CASE("fx.reverb: room glide (FX-825) and the Sixteen reset room", "[fx][reverb]")
{
    const double fs = 44100.0;
    const float knobs[6] = {0.0f, 0.0f, 0.5f, 0.0f, 0.0f, 1.0f};
    auto e = make(fs, knobs);
    CHECK(e->roomCurrent() == 1); // Sixteen: the stored room at once
    e->setParam(1, 1.0f);
    const std::size_t n = 64 * 400;
    (void) run(*e, std::vector<float>(n, 0.0f), std::vector<float>(n, 0.0f));
    CHECK(e->roomTarget() == 100);
    // 99 steps of t ms, t from 20 ms (|Δ| ≥ 30) up to 40 ms: between 99·20 ms and 99·40 ms
    CHECK(e->roomCurrent() > 1);
    (void) run(*e, std::vector<float>(64 * 3000, 0.0f), std::vector<float>(64 * 3000, 0.0f));
    CHECK(e->roomCurrent() == 100);
    auto ref = make(fs, knobs, kQuirkProfileReference);
    CHECK(ref->roomCurrent() == 50); // the reference keeps the previous target until the first block
}

TEST_CASE("fx.reverb: laws — decay anchors, room scale, input gain, spread table", "[fx][reverb]")
{
    const ReverbConstants k;
    for (std::size_t i = 0; i < 5; ++i)
        CHECK(Reverb::t60(k, k.t60v[i]) == Catch::Approx(k.t60s[i]).epsilon(1e-6));
    float last = 0.0f;
    for (int i = 0; i <= 1000; ++i) // monotonic, no blend-edge dip (FX-883)
    {
        const float t = Reverb::t60(k, static_cast<float>(i) / 1000.0f);
        CHECK(t > last);
        last = t;
    }
    CHECK(Reverb::roomScale(50) == Catch::Approx(1.00004).epsilon(1e-4));
    CHECK(Reverb::inputGain(0.0f) == Catch::Approx(0.2f));
    CHECK(Reverb::inputGain(0.5137255f) == Catch::Approx(0.2f));
    CHECK(Reverb::inputGain(1.0f) == Catch::Approx(0.1f).epsilon(1e-5));
    float a[5], b[5];
    Reverb::spreadGains(0.0f, a, b);
    CHECK((a[4] == 0.0f && b[4] == 0.0f)); // comb 5: a plain delay tap at Spread 0
    for (int i = 0; i < 4; ++i)
        CHECK((a[i] == 1.0f && b[i] == 0.0f));
    Reverb::spreadGains(0.5f, a, b);
    CHECK((a[0] == 0.0f && b[0] == 1.0f));
    CHECK(b[1] == Catch::Approx(0.5f));
    CHECK(a[2] == Catch::Approx(0.5f));
    CHECK((a[3] == 1.0f && b[4] == 1.0f));
    Reverb::spreadGains(1.0f, a, b);
    for (int i = 0; i < 5; ++i)
        CHECK((a[i] == 0.0f && b[i] == Catch::Approx(1.0f).epsilon(1e-6)));
    CHECK(Reverb::dampingMaxHz(44100.0) == 18000.0f);
    CHECK(Reverb::dampingMaxHz(32000.0) == 12800.0f);
    CHECK(Reverb::dampingMaxHz(96000.0) == 18000.0f);
}

TEST_CASE("fx.reverb: parameter texts follow the engine laws", "[fx][reverb]")
{
    const auto& reg = core::builtinParams();
    const auto* time = reg.find("send.reverb.time");
    const auto* room = reg.find("master.reverb.room");
    const auto* mix = reg.find("send.reverb.mix");
    REQUIRE(time != nullptr);
    REQUIRE(room != nullptr);
    REQUIRE(mix != nullptr);
    CHECK(! time->lockable); // FX-094
    CHECK(mix->defaultValue == 1.0f);
    CHECK(reg.find("master.reverb.mix")->defaultValue == 0.5f);
    char buf[32];
    room->format(49.0f / 99.0f, buf, sizeof buf);
    CHECK(std::string(buf) == "50");
    const ReverbConstants k;
    for (float v : {0.0f, 0.09f, 0.3f, 49.0f / 99.0f, 0.77f, 1.0f})
    {
        time->format(v, buf, sizeof buf);
        char want[32];
        const double t = Reverb::t60(k, v);
        std::snprintf(want, sizeof want, t < 10.0 ? "%.2f s" : "%.1f s", t);
        CHECK(std::string(buf) == want);
    }
}

TEST_CASE("fx.reverb: memory per instance", "[fx][reverb][memory]")
{
    for (double fs : {44100.0, 48000.0, 96000.0})
    {
        Reverb r;
        r.prepare(fs, N);
        std::printf("fx.reverb: %.0f Hz: %zu bytes per instance (comb rings %d frames), 2 bus slots %.2f MiB\n", fs,
                    r.bytes(), r.combRingFrames(), 2.0 * static_cast<double>(r.bytes()) / 1048576.0);
        CHECK(r.bytes() < (fs <= 48000.0 ? 330000u : 660000u));
    }
}
