// The shared modulation oscillator (spec oscillator.md FX-1300…1308, FX-1380): the app's own random
// table (FX-1305) regenerates from its formula and passes statistical tests, the FX-1380 quirk adapter, the FX-1308
// silent invalid set-ups and the sine's long-run stability. The bit-exact comparison with osc.py is the golden set
// tests/golden/oscillator.
#include "engine/fx/effects/Oscillator.h"
#include "engine/fx/effects/OscillatorRandomTable.h"

#include "core/util/Realtime.h"

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>
#include <cstring>
#include <set>

using namespace engine::fx;

namespace
{
constexpr int N = 64;

std::uint32_t fnv1a(const float* v, int n)
{
    std::uint32_t h = 0x811C9DC5u;
    for (int i = 0; i < n; ++i)
    {
        unsigned char b[4];
        std::memcpy(b, &v[i], 4);
        for (unsigned char x : b)
            h = (h ^ x) * 0x01000193u;
    }
    return h;
}
} // namespace

TEST_CASE("osc_random_stats: the FX-1305 table regenerates bit for bit from its formula", "[fx_color][osc]")
{
    // An independent re-implementation of the documented formula (xorshift32 from 0x5EED1600, advanced before each
    // value; value = (x >> 8)·2^-23 − 1, computed in double here and rounded once — exact either way).
    std::uint32_t x = 0x5EED1600u;
    int mismatches = 0;
    for (int i = 0; i < kOscRandomTableSize; ++i)
    {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        const auto v = static_cast<float>(static_cast<double>(x >> 8) * std::ldexp(1.0, -23) - 1.0);
        if (std::bit_cast<std::uint32_t>(v) != std::bit_cast<std::uint32_t>(kOscRandomTable[i]))
            ++mismatches;
    }
    CHECK(mismatches == 0);
    // The same table as tools/golden-gen/goldenlib/models/oscillator.py random_table() (its FNV-1a over the LE bits,
    // recorded in every oscillator golden) — the table osc.py runs with through `table=`.
    CHECK(fnv1a(kOscRandomTable.data(), kOscRandomTableSize) == 0x54a1c244u);
    CHECK(std::bit_cast<std::uint32_t>(kOscRandomTable[0]) == 0xbd4ff2c0u);
    CHECK(std::bit_cast<std::uint32_t>(kOscRandomTable[1]) == 0x3f5f110eu);
}

TEST_CASE("osc_random_stats: the table is uniform over -1 to 1, unbiased and uncorrelated", "[fx_color][osc]")
{
    double sum = 0.0, sum2 = 0.0, lag = 0.0;
    int bins[16]{};
    for (int i = 0; i < kOscRandomTableSize; ++i)
    {
        const double v = kOscRandomTable[i];
        REQUIRE(v >= -1.0);
        REQUIRE(v < 1.0);
        sum += v;
        sum2 += v * v;
        if (i > 0)
            lag += v * static_cast<double>(kOscRandomTable[i - 1]);
        ++bins[static_cast<int>((v + 1.0) * 8.0)];
    }
    const double n = kOscRandomTableSize;
    const double mean = sum / n, var = sum2 / n - mean * mean;
    CHECK(std::fabs(mean) < 0.02);                    // 3σ of the mean of 8192 uniforms ≈ 0.019
    CHECK(std::fabs(var - 1.0 / 3.0) < 0.02);          // uniform on [-1, 1): variance 1/3
    const double r1 = (lag / (n - 1.0) - mean * mean) / var;
    CHECK(std::fabs(r1) < 0.04);                       // lag-1 correlation, σ ≈ 0.011
    double chi2 = 0.0;
    for (int b : bins)
        chi2 += (b - n / 16.0) * (b - n / 16.0) / (n / 16.0);
    CHECK(chi2 < 37.7);                                // 15 degrees of freedom, p = 0.001
    // Every value of waveform 6 comes from the table.
    std::set<std::uint32_t> table;
    for (float v : kOscRandomTable)
        table.insert(std::bit_cast<std::uint32_t>(v));
    ModOscillator o;
    o.setRandomOffset(100);
    REQUIRE(o.init(ModOscillator::kRandom, 500.0f, 44100.0));
    o.setSmoothingOff(true);
    float l[N], r[N];
    int outside = 0;
    for (int b = 0; b < 200; ++b)
    {
        o.process(l, r, N);
        for (int i = 0; i < N; ++i)
            outside += (table.count(std::bit_cast<std::uint32_t>(l[i])) == 0) + (table.count(std::bit_cast<std::uint32_t>(r[i])) == 0);
    }
    CHECK(outside == 0);
}

TEST_CASE("quirk_oscillator: FX-1380 setFrequency periods at 44,100 Hz (Reference) or fs (Sixteen)", "[fx_color][osc]")
{
    for (double fs : {44100.0, 48000.0, 96000.0})
    {
        ModOscillator ref, six;
        ref.setQuirkProfile(kOscQuirkPeriod44k);
        six.setQuirkProfile(0);
        for (auto* o : {&ref, &six})
        {
            REQUIRE(o->init(ModOscillator::kTriangle, 10.0f, fs));
            CHECK(o->period() == static_cast<float>(fs) / 10.0f - 1.0f); // init always uses fs
            REQUIRE(o->setFrequency(20.0f));
        }
        INFO("fs " << fs);
        CHECK(ref.period() == 44100.0f / 20.0f - 1.0f);
        CHECK(six.period() == static_cast<float>(fs) / 20.0f - 1.0f);
        if (fs == 44100.0)
            CHECK(ref.period() == six.period()); // identical at 44.1 kHz
        // The Sixteen period is the true period of the rate: a 20 Hz triangle repeats every fs/20 − 1 samples.
        float l[N], r[N];
        six.setSmoothingOff(true);
        std::vector<float> y;
        for (int b = 0; b < 160; ++b)
        {
            six.process(l, r, N);
            y.insert(y.end(), l, l + N);
        }
        const int P = static_cast<int>(six.period());
        CHECK(y[100] == y[100 + P]);
    }
}

TEST_CASE("oscillator: FX-1308 invalid set-ups are silent at the offset and never NaN", "[fx_color][osc]")
{
    float l[N], r[N];
    ModOscillator o;
    CHECK_FALSE(o.init(7, 1.0f, 44100.0));
    o.setOffset(0.25f);
    std::fill(l, l + N, 9.0f);
    o.process(l, r, N);
    for (int i = 0; i < N; ++i)
        CHECK((l[i] == 0.25f && r[i] == 0.25f));
    ModOscillator z;
    CHECK_FALSE(z.init(ModOscillator::kSine, 0.0f, 44100.0)); // f = 0 at init
    z.setOffset(-0.5f);
    CHECK_FALSE(z.setPhaseOffset(0.3f));
    CHECK_FALSE(z.resetPhase(0.3f));
    z.process(l, r, N);
    CHECK((l[0] == -0.5f && r[N - 1] == -0.5f));
    ModOscillator fsBad;
    CHECK_FALSE(fsBad.init(ModOscillator::kSine, 1.0f, 0.0));
    fsBad.process(l, r, N);
    CHECK(std::isfinite(l[0]));
    // A fresh object is fully initialised (no uninitialised heap, H036): setFrequency before init stays finite.
    ModOscillator fresh;
    CHECK(fresh.setFrequency(5.0f) == false);
    fresh.process(l, r, N);
    CHECK(std::isfinite(l[0]));
}

TEST_CASE("oscillator: the sine stays within ±1 through 10,000 blocks of renormalisation (FX-1303/1304)",
          "[fx_color][osc]")
{
    ModOscillator o;
    REQUIRE(o.init(ModOscillator::kSine, 3.7f, 44100.0));
    o.setPhaseOffset(0.25f);
    o.setSmoothingOff(true);
    float l[N], r[N];
    float peak = 0.0f, peakR = 0.0f;
    {
        core::rt::ScopedRealtimeThread rt; // no allocation in process()
        for (int b = 0; b < 10000; ++b)
        {
            o.process(l, r, N);
            for (int i = 0; i < N; ++i)
            {
                REQUIRE(std::fabs(l[i]) <= 1.0f);
                peak = std::max(peak, std::fabs(l[i]));
                peakR = std::max(peakR, std::fabs(r[i]));
            }
        }
    }
    CHECK(peak > 0.9999f);
    CHECK(peakR > 0.999f);
    CHECK(peakR < 1.001f);
    // L(n) = sin((n + 2)·w): R leads by a quarter cycle, so R ≈ cos at the same phase.
    ModOscillator q;
    REQUIRE(q.init(ModOscillator::kSine, 100.0f, 44100.0));
    q.setPhaseOffset(0.25f);
    q.setSmoothingOff(true);
    q.process(l, r, N);
    const double w = 2.0 * 3.14159265358979 * 100.0 / 44100.0;
    for (int i = 0; i < N; ++i)
    {
        CHECK(std::fabs(static_cast<double>(l[i]) - std::sin((i + 2) * w)) < 1e-6);
        CHECK(std::fabs(static_cast<double>(r[i]) - std::cos((i + 2) * w)) < 1e-6);
    }
}
