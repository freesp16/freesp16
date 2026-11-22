// Compressor behavioural goldens (a CI gate of the Compressor independent of the expf substitution of the app-maths
// golden in tests/golden/compressor). Spec 02-features/effects/compressor.md: the static curve against the FX-1121
// formula (hard and soft knee, peak and RMS), the attack and release slopes of 8.69 dB per time constant (FX-1123), the
// look-ahead lead of exactly round(0.002·fs) samples with the detector on the undelayed input (FX-1125), and the mix
// law dry 1 − v / wet v·makeup (FX-1124). Analytic references in double; tolerances from the page (gain trace
// ≤ 0.01 dB).
#include "engine/fx/EffectBase.h"
#include "engine/fx/FxShell.h"
#include "engine/fx/effects/Compressor.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <vector>

using engine::fx::effects::Compressor;

namespace
{
constexpr int N = engine::kBlockFrames;
constexpr double kPi = 3.14159265358979323846;

float thrV(int db)
{
    return static_cast<float>(db + 52) / 52.0f;
}
float ratioV(double r)
{
    return static_cast<float>((std::lround(r * 10) - 10) / 290.0);
}
float attackV(double ms) // the page's attack grid
{
    const long i = ms <= 1.0 ? std::lround(ms / 0.05) : std::lround(ms) + 19;
    return static_cast<float>((i - 1) / 1018.0);
}
float releaseV(double ms)
{
    return static_cast<float>((ms - 10.0) / 3990.0);
}

// FX-1121: the static curve y(L) in dB.
double staticOut(double L, double T, double R, double K)
{
    if (K <= 0.0)
        return L > T ? T + (L - T) / R : L;
    if (L <= T - K)
        return L;
    if (L <= T + K)
        return L + (1.0 / R - 1.0) * (L - T + K) * (L - T + K) / (4.0 * K);
    return T + (L - T) / R;
}

struct Run
{
    Compressor c;
    engine::fx::ProcessContext ctx{44100.0, 120.0, true, 0u, {}, 0};
    explicit Run(double fs = 44100.0) { c.prepare(fs, N); }
    // One block of a stereo signal f(i); returns the block's gain trace = out/in on the left channel where |in| > 0.
    void block(const float* inL, const float* inR, float* outL, float* outR)
    {
        std::memcpy(outL, inL, sizeof(float) * N);
        std::memcpy(outR, inR, sizeof(float) * N);
        c.process(outL, outR, ctx);
    }
};

double db(double x)
{
    return 20.0 * std::log10(x);
}
} // namespace

TEST_CASE("compressor_behaviour: the settled gain follows the static curve (FX-1121), peak and RMS, hard and soft knee",
          "[golden][compressor]")
{
    for (int knee : {0, 1})
        for (int detect : {0, 1})
            for (int T : {-40, -20, -6})
                for (double R : {2.0, 4.0, 30.0})
                    for (double level : {0.01, 0.05, 0.2, 0.5, 0.9})
                    {
                        INFO("knee " << knee << " detect " << detect << " T " << T << " R " << R << " level " << level);
                        Run r;
                        r.c.setParam(0, thrV(T));
                        r.c.setParam(1, ratioV(R));
                        r.c.setParam(2, attackV(0.05));
                        r.c.setParam(3, releaseV(10));
                        r.c.setParam(4, 0.0f);
                        r.c.setParam(5, 1.0f);
                        r.c.setOption(Compressor::kOptKnee, knee);
                        r.c.setOption(Compressor::kOptDetect, detect);
                        r.c.reset();
                        alignas(16) float l[N], rr[N], ol[N], orr[N];
                        // peak: DC (level exactly); RMS: a sine whose √2·RMS = level (997 Hz over the 3 ms window)
                        long n = 0;
                        for (int b = 0; b < 200; ++b)
                        {
                            for (int i = 0; i < N; ++i, ++n)
                            {
                                const double s = detect == 1 ? std::sin(2.0 * kPi * 1000.0 * static_cast<double>(n) / 44100.0) : 1.0;
                                l[i] = rr[i] = static_cast<float>(level * s);
                            }
                            r.block(l, rr, ol, orr);
                        }
                        const double L = static_cast<double>(r.c.levelDb());
                        const double want = staticOut(L, T, R, knee ? 18.0 : 0.0) - L;
                        const double got = db(static_cast<double>(r.c.gain()));
                        CHECK(std::abs(got - want) <= 0.01);
                        if (detect == 0)
                            CHECK(std::abs(L - db(level)) <= 1e-4);
                        else
                            CHECK(std::abs(L - db(level)) <= 0.1); // the 3 ms RMS window of a 1 kHz sine
                    }
}

TEST_CASE("compressor_behaviour: attack and release slopes are 8.69 dB per time constant (FX-1123)",
          "[golden][compressor]")
{
    for (double attackMs : {2.0, 10.0, 50.0})
    {
        INFO("attack " << attackMs << " ms");
        Run r;
        r.c.setParam(0, thrV(-50));
        r.c.setParam(1, 1.0f); // 30:1: a deep target, so the slew limit governs
        r.c.setParam(2, attackV(attackMs));
        r.c.setParam(3, releaseV(attackMs * 2.0));
        r.c.setParam(5, 1.0f);
        r.c.reset();
        alignas(16) float l[N], rr[N], ol[N], orr[N];
        for (int i = 0; i < N; ++i)
            l[i] = rr[i] = 0.9f;
        std::vector<double> trace;
        for (int b = 0; b < 300; ++b) // down to the static curve's floor (≈ −47 dB, 5.5 attack times)
        {
            r.block(l, rr, ol, orr);
            for (int i = 0; i < N; ++i)
                trace.push_back(db(static_cast<double>(ol[i]) / 0.9));
        }
        // the fall over one attack time, taken in the middle of the slew-limited stretch
        const int span = static_cast<int>(std::lround(attackMs * 44.1));
        const int a = 64, z = a + span;
        REQUIRE(z < static_cast<int>(trace.size()));
        const double drop = trace[static_cast<std::size_t>(a)] - trace[static_cast<std::size_t>(z)];
        CHECK(std::abs(drop - 8.6859) <= 0.02 * 8.6859);
        // then the release: silence below the threshold, the gain rises 8.69 dB per release time after the hold
        alignas(16) float q[N];
        for (int i = 0; i < N; ++i)
            q[i] = 1e-6f; // −120 dB: target 1, slew-limited rise
        std::vector<double> up;
        for (int b = 0; b < 200; ++b)
        {
            r.block(q, q, ol, orr);
            for (int i = 0; i < N; ++i)
                up.push_back(db(static_cast<double>(ol[i]) / 1e-6));
        }
        const int rs = static_cast<int>(std::lround(attackMs * 2.0 * 44.1));
        const int s0 = 9 * N; // after the hold (≤ 441 samples) and the first ×e step of the target
        REQUIRE(s0 + rs < static_cast<int>(up.size()));
        REQUIRE(up[static_cast<std::size_t>(s0 + rs)] < -0.5); // still slewing
        const double rise = up[static_cast<std::size_t>(s0 + rs)] - up[static_cast<std::size_t>(s0)];
        CHECK(std::abs(rise - 8.6859) <= 0.02 * 8.6859);
    }
}

TEST_CASE("compressor_behaviour: look-ahead lead = round(0.002·fs) samples exactly; the detector sees the undelayed "
          "input (FX-1125)",
          "[golden][compressor]")
{
    for (double fs : {44100.0, 48000.0})
    {
        INFO("fs " << fs);
        const int lead = static_cast<int>(std::lround(0.002 * fs));
        // Two runs on the same input: look-ahead off and on. With it on the audio is the same signal `lead` samples
        // later, while the gain trace is the same (detector undelayed): out_on[i + lead] = g_on[i + lead]·in[i].
        std::vector<float> in(static_cast<std::size_t>(40 * N));
        for (std::size_t i = 0; i < in.size(); ++i)
            in[i] = static_cast<float>((i > 700 ? 0.9 : 0.02) * std::sin(0.05 * static_cast<double>(i)));
        std::vector<float> off, on;
        for (int la : {0, 1})
        {
            Run r(fs);
            r.ctx.sampleRate = fs;
            r.c.setParam(0, thrV(-30));
            r.c.setParam(1, ratioV(8));
            r.c.setParam(2, attackV(1));
            r.c.setParam(3, releaseV(100));
            r.c.setParam(5, 1.0f);
            r.c.setOption(Compressor::kOptLookAhead, la);
            r.c.reset();
            REQUIRE(r.c.latencySamples() == (la ? lead : 0));
            alignas(16) float l[N], rr[N], ol[N], orr[N];
            for (int b = 0; b < 40; ++b)
            {
                std::memcpy(l, in.data() + b * N, sizeof l);
                std::memcpy(rr, l, sizeof rr);
                r.block(l, rr, ol, orr);
                (la ? on : off).insert((la ? on : off).end(), ol, ol + N);
            }
        }
        // the delayed signal: zero for the first `lead` samples, then input × the same gain trace as without
        for (int i = 0; i < lead; ++i)
            CHECK(on[static_cast<std::size_t>(i)] == 0.0f);
        int compared = 0;
        for (std::size_t i = static_cast<std::size_t>(lead); i < on.size(); ++i)
        {
            const float x = in[i - static_cast<std::size_t>(lead)], xo = in[i];
            if (std::abs(x) < 1e-3f || std::abs(xo) < 1e-3f)
                continue;
            const double gOn = static_cast<double>(on[i]) / static_cast<double>(x);
            const double gOff = static_cast<double>(off[i]) / static_cast<double>(xo);
            CHECK(std::abs(db(gOn) - db(gOff)) <= 1e-3);
            ++compared;
        }
        CHECK(compared > 1000);
    }
}

TEST_CASE("compressor_behaviour: mix law dry 1 − v, wet v·makeup (FX-1124), through the shell", "[golden][compressor]")
{
    for (float v : {0.0f, 0.5f, 1.0f})
        for (int gainDb : {0, 6, 50})
        {
            INFO("mix " << v << " gain " << gainDb);
            Compressor c;
            c.prepare(44100.0, N);
            c.setParam(0, thrV(-24));
            c.setParam(1, ratioV(4));
            c.setParam(4, static_cast<float>(gainDb) / 50.0f);
            c.setParam(5, v);
            c.reset();
            engine::fx::ProcessContext ctx{44100.0, 120.0, true, 0u, {}, 0};
            alignas(16) float l[N], r[N], x[N];
            for (int b = 0; b < 30; ++b)
            {
                for (int i = 0; i < N; ++i)
                    x[i] = l[i] = r[i] = 0.5f * static_cast<float>(std::sin(0.03 * (b * N + i)));
                c.process(l, r, ctx);
            }
            const float makeup = static_cast<float>(std::pow(10.0, gainDb / 20.0));
            CHECK(c.telemetry(engine::fx::kTelemetryDry) == 1.0f - v);
            CHECK(std::abs(c.telemetry(engine::fx::kTelemetryWet) - v * makeup) <= 1e-6f * (1.0f + v * makeup));
            // out = (1 − v)·x + v·makeup·(gain·x): the shell's mix with the effect's wet signal y = gain·x
            const float d = c.telemetry(engine::fx::kTelemetryDry), w = c.telemetry(engine::fx::kTelemetryWet);
            for (int i = 0; i < N; ++i)
            {
                const double out = static_cast<double>(x[i] * d) + static_cast<double>(l[i] * w);
                const double want = (1.0 - static_cast<double>(v)) * static_cast<double>(x[i]) +
                                    static_cast<double>(v) * static_cast<double>(makeup) * static_cast<double>(l[i]);
                CHECK(std::abs(out - want) <= 1e-5);
            }
        }
}
