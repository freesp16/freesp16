// Vintage Pitch Shifter (engine/fx/effects/VintagePitch.*): parameter laws, gain staging (unity through the
// converters and the compander at every level, the A/D clip only on hot transients), the pitch actually produced
// (zero-crossing rate of a shifted sine), the pointer/splice law, feedback stability at ±100 %, AGE's dead LSBs, the
// DELAY knob's crossfaded jump, determinism, and the NEON lanes against the scalar reference bit for bit (this file
// mentions APP_FX_HAVE_NEON, so scripts/neon-tests.sh runs it under qemu-arm as a Cortex-A9; on x86-64 that case
// SKIPs). No registry is needed: the device test build links the module libraries without the registrars.
#include "engine/fx/dsp/Numerics.h"
#include "engine/fx/effects/VintagePitch.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace
{
using engine::fx::VintagePitch;
constexpr int N = engine::kBlockFrames;
constexpr double kFs = 44100.0;

struct Noise // xorshift32, uniform in (−a, a)
{
    std::uint32_t s = 0x5EED1600u;
    float next(float a)
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return a * (static_cast<float>(s >> 8) * (2.0f / 16777216.0f) - 1.0f);
    }
};

// Knob fractions of the catalogue's grids (fx_vpitch.cpp).
float pitchV(int st)
{
    return static_cast<float>(st + 12) / 24.0f;
}
float fineV(int ct)
{
    return static_cast<float>(ct + 50) / 100.0f;
}
float fbV(int pct)
{
    return static_cast<float>(pct + 100) / 200.0f;
}

std::unique_ptr<VintagePitch> make(int st, int ct, int delayMs, int fbPct, int agePct)
{
    auto v = std::make_unique<VintagePitch>();
    v->prepare(kFs, N);
    v->setParam(0, pitchV(st));
    v->setParam(1, fineV(ct));
    v->setParam(2, static_cast<float>(delayMs) / 120.0f);
    v->setParam(3, fbV(fbPct));
    v->setParam(4, static_cast<float>(agePct) / 100.0f);
    v->setParam(5, 1.0f);
    v->reset();
    return v;
}

// Runs `seconds` of a generated stereo signal through the effect; returns the wet output (L only, R is checked equal
// for a mono input elsewhere).
template <typename Gen> std::vector<float> run(VintagePitch& v, double seconds, Gen gen)
{
    const int blocks = static_cast<int>(seconds * kFs / N);
    std::vector<float> out;
    out.reserve(static_cast<std::size_t>(blocks * N));
    alignas(16) float l[N], r[N];
    const engine::fx::ProcessContext ctx{kFs, 120.0, true, 0u, {}, 0};
    long n = 0;
    for (int b = 0; b < blocks; ++b)
    {
        for (int i = 0; i < N; ++i, ++n)
            l[i] = r[i] = gen(n);
        v.process(l, r, ctx);
        out.insert(out.end(), l, l + N);
    }
    return out;
}

double rms(const std::vector<float>& x, std::size_t from)
{
    double s = 0;
    for (std::size_t i = from; i < x.size(); ++i)
        s += static_cast<double>(x[i]) * static_cast<double>(x[i]);
    return std::sqrt(s / static_cast<double>(x.size() - from));
}
double peak(const std::vector<float>& x, std::size_t from)
{
    double p = 0;
    for (std::size_t i = from; i < x.size(); ++i)
        p = std::max(p, std::fabs(static_cast<double>(x[i])));
    return p;
}
double db(double x)
{
    return 20.0 * std::log10(std::max(x, 1e-12));
}
// Frequency from rising zero crossings (after a 0.25 s settle).
double frequency(const std::vector<float>& x)
{
    const std::size_t from = static_cast<std::size_t>(0.25 * kFs);
    int crossings = 0;
    std::size_t first = 0, last = 0;
    for (std::size_t i = from + 1; i < x.size(); ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
        {
            if (crossings == 0)
                first = i;
            last = i;
            ++crossings;
        }
    return crossings > 1 ? (crossings - 1) * kFs / static_cast<double>(last - first) : 0.0;
}
auto sine(double hz, double dbfs)
{
    const double a = std::pow(10.0, dbfs / 20.0);
    return [=](long n)
    { return static_cast<float>(a * std::sin(6.283185307179586 * hz * static_cast<double>(n) / kFs)); };
}
} // namespace

TEST_CASE("fx.vpitch: parameter laws (catalogue grids, ratio)", "[fx][vpitch]")
{
    CHECK(VintagePitch::semitones(0.0f) == -12);
    CHECK(VintagePitch::semitones(0.5f) == 0);
    CHECK(VintagePitch::semitones(1.0f) == 12);
    CHECK(VintagePitch::semitones(pitchV(7)) == 7);
    CHECK(VintagePitch::cents(fineV(-13)) == -13);
    CHECK(VintagePitch::delayMs(1.0f) == 120);
    CHECK(VintagePitch::feedbackPct(0.0f) == -100);
    CHECK(VintagePitch::feedbackPct(0.5f) == 0);
    CHECK(VintagePitch::agePct(0.35f) == 35);
    CHECK(VintagePitch::ratioOf(0, 0) == 1.0f);
    CHECK(VintagePitch::ratioOf(12, 0) == 2.0f);
    CHECK(VintagePitch::ratioOf(-12, 0) == 0.5f);
    CHECK(VintagePitch::ratioOf(12, 50) == 2.0f); // the VCO's ceiling: two reads per frame
    CHECK(VintagePitch::ratioOf(7, 0) == Catch::Approx(1.4983071).epsilon(1e-6));
}

TEST_CASE("fx.vpitch: gain staging — unity through converters and compander at every level", "[fx][vpitch]")
{
    // PITCH 0, AGE 0: a delay of half a window through the whole chain. The compander is 2:1 / 1:2 about the same
    // point, so the level comes back whatever it was going in (the converters' noise floor aside).
    for (const double level : {-6.0, -18.0, -30.0, -42.0})
    {
        auto v = make(0, 0, 0, 0, 0);
        auto gen = sine(441.0, level);
        run(*v, 0.5, gen); // settle: the compressor starts at its +45 dB silence gain, so the very first cycle clips
        const auto settled = v->limitHits();
        const auto out = run(*v, 1.0, [&](long n) { return gen(n + static_cast<long>(0.5 * kFs / N) * N); });
        const double got = db(rms(out, 0));
        const double want = level - 3.0103; // a sine's RMS
        INFO("input " << level << " dBFS: output RMS " << got << " dBFS");
        CHECK(std::fabs(got - want) < 0.5);
        CHECK(v->limitHits() == settled);
    }
}

TEST_CASE("fx.vpitch: gain staging — a 0 dBFS drum hit stays inside the converters", "[fx][vpitch]")
{
    // A drum-like hit every 0.25 s: a 60 Hz thump and a click (noise through a 4 kHz one-pole), ≈ 0 dBFS peak, crest
    // factor ≈ 15 dB. The compressor lifts it toward its unity point and the pre-emphasis adds up to +10 dB of treble;
    // kTrim keeps the A/D clip (LIMIT) to the odd frame of an attack. The expander gives the body back within a few
    // tenths of a dB; the click comes back a few dB low — the compander's documented transient mistracking (the
    // compressor's detector hears the full-band pre-emphasised attack, the expander's only what passed the 12 kHz
    // anti-alias filter), so the whole hit sits within 2 dB of its input level.
    Noise noise;
    double lp = 0;
    auto hit = [&](long n)
    {
        const long k = n % static_cast<long>(0.25 * kFs);
        const double t = static_cast<double>(k) / kFs;
        const double body = std::sin(6.283185307179586 * 60.0 * t) * std::exp(-t / 0.06);
        lp += 0.43 * (static_cast<double>(noise.next(1.0f)) - lp);
        const double click = 2.0 * lp * std::exp(-t / 0.008);
        return static_cast<float>(0.7 * body + 0.3 * click);
    };
    auto v = make(0, 0, 0, 0, 0);
    const auto out = run(*v, 3.0, hit);
    Noise again;
    noise = again;
    lp = 0;
    std::vector<float> in;
    for (long n = 0; n < static_cast<long>(out.size()); ++n)
        in.push_back(hit(n));
    const auto from = static_cast<std::size_t>(0.5 * kFs);
    const double frames = 3.0 * 33488.0;
    INFO("LIMIT frames " << v->limitHits() << ", in peak " << db(peak(in, from)) << " rms " << db(rms(in, from))
                         << ", out peak " << db(peak(out, from)) << " rms " << db(rms(out, from)));
    CHECK(static_cast<double>(v->limitHits()) < 0.002 * frames);
    CHECK(std::fabs(db(rms(out, from)) - db(rms(in, from))) < 2.0);
    CHECK(peak(out, from) < 1.0);
}

TEST_CASE("fx.vpitch: the output is pitched by the ratio", "[fx][vpitch]")
{
    for (const int st : {-12, -5, 7, 12})
    {
        auto v = make(st, 0, 0, 0, 0);
        const auto out = run(*v, 2.0, sine(330.0, -12.0));
        const double want = 330.0 * std::pow(2.0, st / 12.0);
        INFO("PITCH " << st << " st: " << frequency(out) << " Hz, want " << want);
        CHECK(std::fabs(frequency(out) / want - 1.0) < 0.01);
        CHECK(v->splices() > 0);
    }
}

TEST_CASE("fx.vpitch: the pointer stays inside its window and splices at |ratio − 1|·FREF/1024", "[fx][vpitch]")
{
    auto v = make(7, 0, 0, 0, 0);
    alignas(16) float l[N]{}, r[N]{};
    const engine::fx::ProcessContext ctx{kFs, 120.0, true, 0u, {}, 0};
    const int blocks = static_cast<int>(2.0 * kFs / N);
    for (int b = 0; b < blocks; ++b)
    {
        v->process(l, r, ctx);
        CHECK(v->pointerDelay() >= 0);
        CHECK(v->pointerDelay() <= 1024);
        const float t = v->telemetry(VintagePitch::kTelPointer);
        CHECK(t >= 0.0f);
        CHECK(t <= 1.0f);
    }
    const double want = (static_cast<double>(VintagePitch::ratioOf(7, 0)) - 1.0) * 33488.0 / 1024.0 * 2.0;
    INFO("splices " << v->splices() << ", want ≈ " << want);
    CHECK(std::fabs(static_cast<double>(v->splices()) - want) < 0.05 * want + 1.0);
}

TEST_CASE("fx.vpitch: feedback at ±100 % stays bounded (the FB LEV ceiling and the compander)", "[fx][vpitch]")
{
    for (const int fb : {100, -100})
        for (const int st : {0, 7, -12})
        {
            auto v = make(st, 0, 60, fb, 50);
            Noise noise;
            const auto out =
                run(*v, 6.0, [&](long n) { return n < static_cast<long>(0.2 * kFs) ? noise.next(0.5f) : 0.0f; });
            INFO("FEEDBACK " << fb << " %, PITCH " << st << ": peak " << peak(out, 0));
            CHECK(peak(out, 0) < 2.0);
            for (float x : out)
                REQUIRE(std::isfinite(x));
        }
}

TEST_CASE("fx.vpitch: AGE kills LSBs from 60 % and is deterministic", "[fx][vpitch]")
{
    CHECK(make(0, 0, 0, 0, 0)->deadLsbs() == 0);
    CHECK(make(0, 0, 0, 0, 59)->deadLsbs() == 0);
    CHECK(make(0, 0, 0, 0, 60)->deadLsbs() == 1);
    CHECK(make(0, 0, 0, 0, 90)->deadLsbs() == 2);
    CHECK(make(0, 0, 0, 0, 100)->deadLsbs() == 3);
    auto a = make(-5, 13, 30, 40, 80);
    auto b = make(-5, 13, 30, 40, 80);
    const auto x = run(*a, 0.5, sine(220.0, -6.0));
    const auto y = run(*b, 0.5, sine(220.0, -6.0));
    CHECK(std::memcmp(x.data(), y.data(), x.size() * sizeof(float)) == 0);
}

TEST_CASE("fx.vpitch: DELAY moves the read tap by the knob, through a crossfade", "[fx][vpitch]")
{
    auto v = make(0, 0, 0, 0, 0);
    alignas(16) float l[N]{}, r[N]{};
    const engine::fx::ProcessContext ctx{kFs, 120.0, true, 0u, {}, 0};
    for (int b = 0; b < 20; ++b)
        v->process(l, r, ctx);
    const auto before = v->splices();
    v->setParam(2, 0.5f); // 60 ms
    for (int b = 0; b < 4; ++b)
        v->process(l, r, ctx);
    CHECK(v->presetFrames() == static_cast<int>(std::lround(0.060 * 33488.0)));
    CHECK(v->splices() == before + 1);
    // turned again while that crossfade runs: deferred until it is over (one splice per crossfade)
    v->setParam(2, 0.25f);
    v->process(l, r, ctx);
    CHECK(v->splices() == before + 1);
    for (int b = 0; b < 40; ++b)
        v->process(l, r, ctx);
    CHECK(v->presetFrames() == static_cast<int>(std::lround(0.030 * 33488.0)));
    CHECK(v->splices() == before + 2);
}

TEST_CASE("fx.vpitch: neon_vs_scalar — processNeon equals processScalar bit for bit", "[fx][vpitch][simd]")
{
#if ! APP_FX_HAVE_NEON
    SKIP("no NEON on this host");
#else
    struct Config
    {
        int st, ct, delay, fb, age;
    };
    for (const auto c : {Config{7, 0, 0, 0, 35}, Config{-12, 0, 30, 60, 100}, Config{0, 9, 120, -80, 0},
                         Config{12, -50, 10, 100, 70}, Config{-3, 21, 0, -100, 55}})
    {
        auto s = make(c.st, c.ct, c.delay, c.fb, c.age);
        auto v = make(c.st, c.ct, c.delay, c.fb, c.age);
        Noise noise;
        alignas(16) float sl[N], sr[N], vl[N], vr[N];
        for (int b = 0; b < 400; ++b)
        {
            for (int i = 0; i < N; ++i)
            {
                sl[i] = vl[i] = noise.next(b % 50 < 30 ? 0.9f : 0.02f);
                sr[i] = vr[i] = noise.next(0.6f);
            }
            if (b == 200) // knobs move mid-run: the control path too
                for (auto* e : {s.get(), v.get()})
                {
                    e->setParam(0, pitchV(-c.st / 2));
                    e->setParam(2, 0.4f);
                    e->setParam(4, 0.9f);
                }
            s->processScalar(sl, sr);
            v->processNeon(vl, vr);
            INFO("config st " << c.st << " block " << b);
            REQUIRE(std::memcmp(sl, vl, sizeof sl) == 0);
            REQUIRE(std::memcmp(sr, vr, sizeof sr) == 0);
        }
        CHECK(s->splices() == v->splices());
        CHECK(s->limitHits() == v->limitHits());
    }
#endif
}
