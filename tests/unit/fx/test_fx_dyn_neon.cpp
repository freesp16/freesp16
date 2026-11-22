// neon_vs_scalar_compressor / neon_vs_scalar_ducker (bit-exact on normal inputs): the NEON
// kernels of engine/fx/dsp/compressor_kernels.h and both effects' processNeon() against their scalar references, bit
// for bit, over many blocks and parameter sets. On a host without NEON (x86-64) these SKIP; scripts/neon-tests.sh
// runs them under qemu-arm as a Cortex-A9 (this file mentions APP_FX_HAVE_NEON, so tests/neon picks it up). No
// registry or fakes are used here: the device test build links the module libraries without the registrars.
#include "engine/fx/dsp/Numerics.h"
#include "engine/fx/dsp/compressor_kernels.h"
#include "engine/fx/effects/Compressor.h"
#include "engine/fx/effects/Ducker.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace
{
constexpr int N = engine::kBlockFrames;

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

[[maybe_unused]] bool same(const float* a, const float* b, int n)
{
    return std::memcmp(a, b, sizeof(float) * static_cast<std::size_t>(n)) == 0;
}
} // namespace

TEST_CASE("fx.dyn: neon_vs_scalar_compressor — kernels and processNeon equal the scalar reference bit for bit",
          "[fx][compressor][simd]")
{
#if ! APP_FX_HAVE_NEON
    SKIP("no NEON on this host");
#else
    namespace ck = engine::fx::dsp::compressor;
    Noise rng;
    for (int n = 0; n <= 67; ++n)
        for (int rep = 0; rep < 8; ++rep)
        {
            std::vector<float> l(static_cast<std::size_t>(n + 1)), r(l.size()), g(l.size()), c(l.size()), f(l.size());
            const float amp = rep == 0 ? 1e-9f : (rep == 1 ? 1.0f : 0.5f);
            for (int i = 0; i <= n; ++i)
            {
                l[static_cast<std::size_t>(i)] = rng.next(amp);
                r[static_cast<std::size_t>(i)] = rng.next(amp);
                g[static_cast<std::size_t>(i)] = rng.next(1.0f);
                f[static_cast<std::size_t>(i)] = rng.next(1.0f);
                c[static_cast<std::size_t>(i)] = 1.0f - f[static_cast<std::size_t>(i)];
            }
            if (rep == 3 && n > 4) // ties: the first maximum wins
                l[2] = l[static_cast<std::size_t>(n - 1)] = 2.0f;
            int pa = 0, pb = 0;
            const float a = ck::scalar::peakDetect(l.data(), r.data(), n, pa);
            const float b = ck::neon::peakDetect(l.data(), r.data(), n, pb);
            INFO("n " << n << " rep " << rep);
            CHECK(engine::fx::dsp::bitsOf(a) == engine::fx::dsp::bitsOf(b));
            CHECK(pa == pb);
            std::vector<float> o1(l.size()), o2(l.size()), o3(l.size()), o4(l.size());
            ck::scalar::applyGain(g.data(), l.data(), r.data(), o1.data(), o2.data(), n);
            ck::neon::applyGain(g.data(), l.data(), r.data(), o3.data(), o4.data(), n);
            CHECK(same(o1.data(), o3.data(), n));
            CHECK(same(o2.data(), o4.data(), n));
            auto x1 = l, x2 = l;
            ck::scalar::crossfade(x1.data(), x1.data(), r.data(), c.data(), f.data(), n);
            ck::neon::crossfade(x2.data(), x2.data(), r.data(), c.data(), f.data(), n);
            CHECK(same(x1.data(), x2.data(), n));
        }
    // whole effect: peak/RMS × knee × look-ahead toggles, parameter changes every few blocks
    for (int config = 0; config < 8; ++config)
    {
        INFO("config " << config);
        auto s = std::make_unique<engine::fx::effects::Compressor>();
        auto v = std::make_unique<engine::fx::effects::Compressor>();
        s->prepare(44100.0, N);
        v->prepare(44100.0, N);
        const int detect = config & 1, knee = (config >> 1) & 1, la = (config >> 2) & 1;
        for (auto* e : {s.get(), v.get()})
        {
            e->setParam(0, 0.4f);
            e->setParam(1, 0.3f);
            e->setParam(2, 0.01f);
            e->setParam(3, 0.02f);
            e->setParam(4, 0.1f);
            e->setParam(5, 0.8f);
            e->setOption(engine::fx::effects::Compressor::kOptDetect, detect);
            e->setOption(engine::fx::effects::Compressor::kOptKnee, knee);
            e->setOption(engine::fx::effects::Compressor::kOptLookAhead, la);
            e->reset();
        }
        Noise in;
        for (int b = 0; b < 300; ++b)
        {
            alignas(16) float l1[N], r1[N], l2[N], r2[N];
            const float amp = (b / 30) % 2 == 0 ? 0.9f : 0.02f;
            for (int i = 0; i < N; ++i)
            {
                l1[i] = l2[i] = in.next(amp);
                r1[i] = r2[i] = in.next(amp);
            }
            if (b % 50 == 25)
                for (auto* e : {s.get(), v.get()})
                {
                    e->setParam(0, static_cast<float>(b % 7) / 7.0f);
                    e->setOption(engine::fx::effects::Compressor::kOptLookAhead, (b / 50) % 2);
                }
            s->processScalar(l1, r1);
            v->processNeon(l2, r2);
            REQUIRE(same(l1, l2, N));
            REQUIRE(same(r1, r2, N));
        }
    }
#endif
}

TEST_CASE("fx.dyn: neon_vs_scalar_ducker — processNeon equals the scalar reference bit for bit", "[fx][ducker][simd]")
{
#if ! APP_FX_HAVE_NEON
    SKIP("no NEON on this host");
#else
    for (int config = 0; config < 6; ++config)
    {
        INFO("config " << config);
        auto s = std::make_unique<engine::fx::effects::Ducker>();
        auto v = std::make_unique<engine::fx::effects::Ducker>();
        for (auto* e : {s.get(), v.get()})
        {
            e->prepare(44100.0, N);
            e->setParam(0, 0.001f * static_cast<float>(config * 7 + 1));
            e->setParam(1, static_cast<float>(config) / 5.0f);
            e->setParam(2, 0.002f * static_cast<float>(config + 1));
            e->setParam(3, 1.0f - static_cast<float>(config) / 5.0f);
            e->setParam(5, config == 2 ? 0.5f : 1.0f);
            e->setOption(engine::fx::effects::Ducker::kOptSync, config == 5 ? 1 : 0);
            e->reset();
        }
        Noise in;
        for (int b = 0; b < 400; ++b)
        {
            alignas(16) float l1[N], r1[N], l2[N], r2[N];
            for (int i = 0; i < N; ++i)
            {
                l1[i] = l2[i] = in.next(0.9f);
                r1[i] = r2[i] = in.next(0.9f);
            }
            const bool hit = b % 9 == 1 || b % 23 == 4;
            const int off = (b * 13) % 64;
            s->processScalar(l1, r1, 120.0f, hit, off);
            v->processNeon(l2, r2, 120.0f, hit, off);
            REQUIRE(same(l1, l2, N));
            REQUIRE(same(r1, r2, N));
        }
    }
#endif
}
