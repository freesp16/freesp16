// neon_vs_scalar_delay / _delayBus / _reverb (bit-exact on normal inputs): each effect's
// processNeon() against its processScalar() reference, bit for bit, over many blocks with parameter, SYNC, tempo and
// reset events in both quirk profiles, plus the kernels of engine/fx/dsp/{delay,reverb}_kernels.h on their own. On a
// host without NEON (x86-64) these SKIP; scripts/neon-tests.sh runs them under qemu-arm as a Cortex-A9 (this file
// mentions APP_FX_HAVE_NEON, so tests/neon picks it up). No registry is used: the device test build links the module
// libraries without the registrars.
#include "core/util/Realtime.h"
#include "engine/fx/FxShell.h"
#include "engine/fx/dsp/Numerics.h"
#include "engine/fx/dsp/delay_kernels.h"
#include "engine/fx/dsp/reverb_kernels.h"
#include "engine/fx/effects/InsertDelay.h"
#include "engine/fx/effects/Reverb.h"
#include "engine/fx/effects/SendDelay.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <memory>

namespace
{
constexpr int N = engine::kBlockFrames;

struct Noise // xorshift32, uniform in (−a, a)
{
    std::uint32_t s = 0x5EED1600u;
    std::uint32_t bits()
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    float next(float a) { return a * (static_cast<float>(bits() >> 8) * (2.0f / 16777216.0f) - 1.0f); }
    float unit() { return static_cast<float>(bits() >> 8) / 16777216.0f; }
};

[[maybe_unused]] bool same(const float* a, const float* b, int n)
{
    return std::memcmp(a, b, sizeof(float) * static_cast<std::size_t>(n)) == 0;
}

// Drives two instances of E with the same inputs and events, one through processScalar, one through processNeon.
template <typename E, typename Step>
[[maybe_unused]] void compare(std::uint32_t quirks, bool hasSync, int blocks, Step&& step)
{
#if APP_FX_HAVE_NEON
    // As on the engine's audio thread (FX-055, NFR-021): FPSCR.FZ makes scalar VFP flush denormals like NEON, so the
    // long feedback tails stay bit-equal (bit-exact on normal inputs).
    core::rt::enableFlushToZero();
    auto a = std::make_unique<E>(), b = std::make_unique<E>();
    for (E* e : {a.get(), b.get()})
    {
        e->prepare(44100.0, N);
        e->setQuirkProfile(quirks);
    }
    Noise rng;
    float bpm = 120.0f;
    alignas(16) float al[N], ar[N], bl[N], br[N];
    for (int blk = 0; blk < blocks; ++blk)
    {
        if (blk % 23 == 0) // new parameters (and sometimes SYNC, tempo, a reset)
        {
            float k[6];
            for (float& v : k)
                v = rng.unit();
            k[0] *= 0.08f; // short times, so echoes and gate jumps happen inside the run
            const int sync = hasSync ? static_cast<int>(rng.bits() & 1u) : -1;
            const bool reset = (rng.bits() & 3u) == 0u || blk == 0;
            bpm = 60.0f + 180.0f * rng.unit();
            for (E* e : {a.get(), b.get()})
            {
                for (int i = 0; i < 6; ++i)
                    e->setParam(i, k[i]);
                if (sync >= 0)
                    e->setOption(0, sync);
                if (reset)
                    e->reset();
            }
        }
        const float amp = blk % 50 < 25 ? 0.8f : 1e-3f;
        for (int i = 0; i < N; ++i)
        {
            al[i] = bl[i] = rng.next(amp);
            ar[i] = br[i] = rng.next(amp);
        }
        step(*a, *b, al, ar, bl, br, bpm);
        INFO("block " << blk << " quirks " << quirks);
        REQUIRE(same(al, bl, N));
        REQUIRE(same(ar, br, N));
    }
#else
    (void) quirks;
    (void) hasSync;
    (void) blocks;
    (void) step;
#endif
}
} // namespace

TEST_CASE("fx.dly: neon_vs_scalar_delay — processNeon equals processScalar bit for bit", "[fx][delay][simd]")
{
#if ! APP_FX_HAVE_NEON
    SKIP("no NEON on this host");
#else
    for (auto q : {engine::fx::kQuirkProfileSixteen, engine::fx::kQuirkProfileReference})
        compare<engine::fx::InsertDelay>(q, true, 1500,
                                         [](auto& a, auto& b, float* al, float* ar, float* bl, float* br, float bpm)
                                         {
                                             a.processScalar(al, ar, bpm);
                                             b.processNeon(bl, br, bpm);
                                         });
#endif
}

TEST_CASE("fx.dly: neon_vs_scalar_delayBus — processNeon equals processScalar bit for bit", "[fx][delayBus][simd]")
{
#if ! APP_FX_HAVE_NEON
    SKIP("no NEON on this host");
#else
    for (auto q : {engine::fx::kQuirkProfileSixteen, engine::fx::kQuirkProfileReference})
        compare<engine::fx::SendDelay>(q, true, 1500,
                                       [](auto& a, auto& b, float* al, float* ar, float* bl, float* br, float bpm)
                                       {
                                           a.processScalar(al, ar, bpm);
                                           b.processNeon(bl, br, bpm);
                                       });
#endif
}

TEST_CASE("fx.dly: neon_vs_scalar_reverb — processNeon equals processScalar bit for bit", "[fx][reverb][simd]")
{
#if ! APP_FX_HAVE_NEON
    SKIP("no NEON on this host");
#else
    for (auto q : {engine::fx::kQuirkProfileSixteen, engine::fx::kQuirkProfileReference})
        compare<engine::fx::Reverb>(q, false, 1500,
                                    [](auto& a, auto& b, float* al, float* ar, float* bl, float* br, float)
                                    {
                                        a.processScalar(al, ar);
                                        b.processNeon(bl, br);
                                    });
#endif
}

TEST_CASE("fx.dly: neon_vs_scalar kernels — stepped filter, all-pass, FDN", "[fx][delay][reverb][simd]")
{
#if ! APP_FX_HAVE_NEON
    SKIP("no NEON on this host");
#else
    namespace d = engine::fx::dsp::delay;
    namespace rv = engine::fx::dsp::reverb;
    Noise rng;
    for (auto kind : {d::SteppedIir1::Kind::LowPass, d::SteppedIir1::Kind::HighPass})
    {
        d::SteppedIir1 a, b;
        a.prepare(kind, 44100.0, 1000, kind == d::SteppedIir1::Kind::LowPass ? 2000.0 : 1.0,
                  kind == d::SteppedIir1::Kind::LowPass ? 20000.0 : 1000.0);
        b = a;
        for (int blk = 0; blk < 400; ++blk)
        {
            if (blk % 37 == 0)
            {
                const auto step = static_cast<std::uint32_t>(1 + (rng.bits() % 1000u));
                a.setStep(step);
                b.setStep(step);
            }
            const int n = 1 + static_cast<int>(rng.bits() % 64u);
            alignas(16) float l1[N], r1[N], l2[N], r2[N];
            for (int i = 0; i < n; ++i)
            {
                l1[i] = l2[i] = rng.next(1.0f);
                r1[i] = r2[i] = rng.next(1.0f);
            }
            a.process<false>(l1, r1, n);
            b.process<true>(l2, r2, n);
            REQUIRE(same(l1, l2, n));
            REQUIRE(same(r1, r2, n));
        }
    }
    // The reverb kernels: the diffusion all-pass and the 8-line FDN (fixed lines: block copies; gliding lines: the
    // interpolated read), at full and partial block sizes.
    {
        auto pa = std::make_unique<rv::AllPass>(), pb = std::make_unique<rv::AllPass>();
        auto fa = std::make_unique<rv::Fdn8>(), fb = std::make_unique<rv::Fdn8>();
        const float maxDelay[rv::Fdn8::kLines] = {2200, 1400, 2950, 2000, 2450, 1600, 2650, 1800};
        const float gains[rv::Fdn8::kLines] = {0.0f, 0.5f, 0.5f, 0.5f, 0.0f, 0.5f, 0.5f, 0.5f};
        for (auto* p : {pa.get(), pb.get()})
            p->prepare(301, 0.7f);
        for (auto* f : {fa.get(), fb.get()})
        {
            f->prepare(maxDelay);
            f->setInputGains(gains);
            for (int i = 0; i < rv::Fdn8::kLines; ++i)
                f->setFilter(i, 0.3f, 0.34f);
        }
        float d0[rv::Fdn8::kLines], d1[rv::Fdn8::kLines];
        for (int i = 0; i < rv::Fdn8::kLines; ++i)
            d0[i] = d1[i] = static_cast<float>(static_cast<int>(0.5f * maxDelay[i]));
        for (int blk = 0; blk < 1200; ++blk)
        {
            const int n = blk % 5 == 0 ? 1 + static_cast<int>(rng.bits() % 64u) : N;
            for (int i = 0; i < rv::Fdn8::kLines; ++i)
            {
                d0[i] = d1[i];
                if (i % 2 == 1 || blk % 97 < 20) // odd lines always move (modulated), even ones glide now and then
                    d1[i] = d0[i] + (rng.unit() - 0.5f) * 0.02f * static_cast<float>(n);
                if (d1[i] < 100.0f || d1[i] > maxDelay[i] - 2.0f)
                    d1[i] = 0.5f * maxDelay[i];
            }
            if (blk % 41 == 0)
                for (int i = 0; i < rv::Fdn8::kLines; ++i)
                {
                    const float a = 0.8f * rng.unit(), b = 0.35f * (1.0f - a);
                    fa->setFilter(i, a, b);
                    fb->setFilter(i, a, b);
                }
            alignas(16) float xr[N], l1[N], l2[N], ol1[N], or1[N], ol2[N], or2[N];
            const float amp = blk % 300 < 150 ? 0.5f : 0.0f;
            for (int i = 0; i < n; ++i)
            {
                l1[i] = l2[i] = rng.next(amp);
                xr[i] = rng.next(amp);
            }
            pa->process<false>(l1, n);
            pb->process<true>(l2, n);
            REQUIRE(same(l1, l2, n));
            fa->process<false>(l1, xr, ol1, or1, d0, d1, n);
            fb->process<true>(l2, xr, ol2, or2, d0, d1, n);
            INFO("block " << blk << " n " << n);
            REQUIRE(same(ol1, ol2, n));
            REQUIRE(same(or1, or2, n));
        }
    }
#endif
}
