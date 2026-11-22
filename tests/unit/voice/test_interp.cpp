// Interpolation kernels (PLY-021): the NEON Hermite against its
// scalar reference (≤ 1e-6), exactness properties, the Interpolator contract (position returned, edges read as zero
// without padding — ASan would catch an out-of-bounds read) and the sinc16 quality mode.
#include "VoiceHarness.h"

#include "engine/voice/InterpKernels.h"
#include "engine/voice/Interpolators.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <random>
#include <vector>

namespace k = engine::voice::kernels;
using Catch::Matchers::WithinAbs;

namespace
{
std::vector<float> noise(int n, unsigned seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> d(-1.0f, 1.0f);
    std::vector<float> v(static_cast<std::size_t>(n));
    for (auto& x : v)
        x = d(rng);
    return v;
}
} // namespace

TEST_CASE("voice.hermite: weights sum to one and hit the taps at fraction 0 and 1", "[voice][interp]")
{
    for (float x = 0.0f; x <= 1.0f; x += 1.0f / 64.0f)
    {
        const auto w = k::hermiteWeights(x);
        CHECK_THAT(static_cast<double>(w.wm1 + w.w0 + w.w1 + w.w2), WithinAbs(1.0, 1e-6));
    }
    const auto w0 = k::hermiteWeights(0.0f);
    CHECK(w0.w0 == 1.0f);
    CHECK(w0.wm1 == 0.0f);
    CHECK(w0.w1 == 0.0f);
    CHECK(w0.w2 == 0.0f);
    const auto w1 = k::hermiteWeights(1.0f);
    CHECK_THAT(static_cast<double>(w1.w1), WithinAbs(1.0, 1e-7));
}

TEST_CASE("voice.hermite: a linear ramp is reproduced exactly at any fraction and rate", "[voice][interp]")
{
    voicetest::TestBuffer b(voicetest::ramp(400, -1.0f, 0.005f));
    const float* src[1] = {b.get()->ch[0]};
    for (const double rate : {1.0, 1.0594630943592953, 0.5, 2.0, -1.0, -0.73})
    {
        std::vector<float> out(64);
        float* dst[1] = {out.data()};
        const double pos = rate > 0 ? 10.37 : 380.61;
        k::hermite4(src, 1, k::toFixed(pos), k::toFixed(rate), dst, 64);
        for (int i = 0; i < 64; ++i)
        {
            const double p = k::fromFixed(k::toFixed(pos) + i * k::toFixed(rate));
            CHECK_THAT(static_cast<double>(out[static_cast<std::size_t>(i)]), WithinAbs(-1.0 + 0.005 * p, 2e-6));
        }
    }
}

TEST_CASE("voice.hermite: NEON matches the scalar reference within 1e-6", "[voice][interp][neon]")
{
#if APP_ENGINE_HAVE_NEON
    const auto l = noise(5000, 7), r = noise(5000, 8);
    voicetest::TestBuffer b(l, r);
    const float* src[2] = {b.get()->ch[0], b.get()->ch[1]};
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> rates(-4.0, 4.0);
    double worst = 0.0;
    for (int trial = 0; trial < 400; ++trial)
    {
        double rate = rates(rng);
        if (trial % 7 == 0)
            rate = trial % 2 ? 1.0 : -1.0; // exact steps with a fraction
        if (std::fabs(rate) < 0.01)
            rate = 0.25;
        const int frames = 1 + trial % 67;
        const double pos = rate > 0 ? 100.0 + trial * 3.137 : 4800.0 - trial * 3.137;
        for (int channels = 1; channels <= 2; ++channels)
        {
            std::vector<float> s0(80), s1(80), v0(80), v1(80);
            float* ds[2] = {s0.data(), s1.data()};
            float* dv[2] = {v0.data(), v1.data()};
            k::scalar::hermite4(src, channels, k::toFixed(pos), k::toFixed(rate), ds, frames);
            k::neon::hermite4(src, channels, k::toFixed(pos), k::toFixed(rate), dv, frames);
            for (int i = 0; i < frames; ++i)
                for (int c = 0; c < channels; ++c)
                {
                    const double e = std::fabs(static_cast<double>((c ? v1 : v0)[static_cast<std::size_t>(i)])
                                               - static_cast<double>((c ? s1 : s0)[static_cast<std::size_t>(i)]));
                    worst = e > worst ? e : worst;
                }
        }
    }
    INFO("worst |NEON − scalar| = " << worst);
    CHECK(worst <= 1e-6);
#else
    SUCCEED("no NEON on this host: the scalar reference is the active kernel");
#endif
}

TEST_CASE("voice.hermite: the copy fast path equals the interpolated path at rate ±1", "[voice][interp]")
{
    const auto l = noise(300, 3);
    voicetest::TestBuffer b(l);
    const float* src[1] = {b.get()->ch[0]};
    std::vector<float> a(64), c(64);
    float* da[1] = {a.data()};
    float* dc[1] = {c.data()};
    k::copy(src, 1, 20, 1, dc, 64);
    k::hermite4(src, 1, k::toFixed(20.0), k::kOne, da, 64);
    for (int i = 0; i < 64; ++i)
        CHECK(a[static_cast<std::size_t>(i)] == c[static_cast<std::size_t>(i)]);
    k::copy(src, 1, 250, -1, dc, 61);
    k::hermite4(src, 1, k::toFixed(250.0), -k::kOne, da, 61);
    for (int i = 0; i < 61; ++i)
        CHECK(a[static_cast<std::size_t>(i)] == c[static_cast<std::size_t>(i)]);
}

TEST_CASE("voice.interp: Interpolator::render returns the new position and reads zero outside the buffer",
          "[voice][interp]")
{
    engine::voice::Hermite4 h;
    engine::voice::Sinc16 s;
    engine::voice::Linear lin;
    CHECK(h.taps() == 4);
    CHECK(s.taps() == 16);
    CHECK(lin.taps() == 2);
    // An unpadded heap buffer: any read outside [0, 32) is an ASan error.
    std::vector<float> data(32, 1.0f);
    const float* src[1] = {data.data()};
    std::vector<float> out(100);
    float* dst[1] = {out.data()};
    for (engine::voice::Interpolator* ip : {static_cast<engine::voice::Interpolator*>(&h),
                                             static_cast<engine::voice::Interpolator*>(&s),
                                             static_cast<engine::voice::Interpolator*>(&lin)})
    {
        const double end = ip->render(src, 1, 32, -10.25, 0.5, dst, 100);
        CHECK_THAT(static_cast<double>(end), WithinAbs(-10.25 + 50.0, 1e-9));
        CHECK(out[0] == 0.0f);                                      // far before the start: silence
        CHECK_THAT(static_cast<double>(out[50]), WithinAbs(1.0, 0.02));                  // inside (position 14.75): the DC value
        const double back = ip->render(src, 1, 32, 40.0, -1.5, dst, 40);
        CHECK_THAT(static_cast<double>(back), WithinAbs(-20.0, 1e-9));
        CHECK(out[0] == 0.0f);
    }
}

TEST_CASE("voice.sinc16: transposition up keeps aliasing far below hermite4", "[voice][interp][sinc]")
{
    // An 18 kHz tone played a fifth up (rate 1.5): the result at 27 kHz folds back to 17.1 kHz. A band-limited
    // interpolator removes it; Hermite leaves a large alias.
    const int n = 8192;
    voicetest::TestBuffer b(voicetest::sine(n, 18000.0));
    const float* src[1] = {b.get()->ch[0]};
    auto energy = [&](engine::voice::Interpolator& ip) {
        std::vector<float> out(2048);
        float* dst[1] = {out.data()};
        ip.render(src, 1, n, 1000.0, 1.5, dst, 2048);
        double e = 0;
        for (float x : out)
            e += static_cast<double>(x) * static_cast<double>(x);
        return e / 2048.0;
    };
    engine::voice::Hermite4 h;
    engine::voice::Sinc16 s;
    const double eh = energy(h), es = energy(s);
    INFO("alias energy hermite4 " << 10 * std::log10(eh) << " dB, sinc16 " << 10 * std::log10(es) << " dB");
    CHECK(es < eh * 0.01); // ≥ 20 dB less
    CHECK(10 * std::log10(es) < -30.0);
}

TEST_CASE("voice.sinc16: a low tone is reproduced accurately at fractional positions", "[voice][interp][sinc]")
{
    const int n = 4096;
    voicetest::TestBuffer b(voicetest::sine(n, 1000.0));
    const float* src[1] = {b.get()->ch[0]};
    engine::voice::Sinc16 s;
    std::vector<float> out(1000);
    float* dst[1] = {out.data()};
    s.render(src, 1, n, 500.3, 0.77, dst, 1000);
    double worst = 0;
    for (int i = 0; i < 1000; ++i)
    {
        const double p = 500.3 + 0.77 * i;
        worst = std::max(worst, std::fabs(static_cast<double>(out[static_cast<std::size_t>(i)]) - std::sin(2 * M_PI * 1000.0 * p / 44100.0)));
    }
    INFO("worst error " << 20 * std::log10(worst) << " dBFS");
    CHECK(worst < 1e-3); // < −60 dBFS
}
