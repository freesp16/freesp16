// NEON ↔ scalar parity of the AFS digital model (bit-exact on normal inputs): each kernel of
// engine/fx/dsp/afs_ladder.h (half-band up / down 4-wide, the L/R-lane ladder, the L/R-lane high-pass) and the whole
// ModelProcessor (with modulation, slope switch, HP on/off, resets) give the same bits with the NEON path and with the
// scalar reference. On x86-64 hosts the NEON branch does not exist and the tests SKIP; scripts/neon-tests.sh runs them
// under qemu-arm as a Cortex-A9 (it picks up files that mention __ARM_NEON / APP_FX_HAVE_NEON).
#include "tests/unit/afs/AfsTestUtil.h"

#include "engine/fx/dsp/Numerics.h"
#include "engine/fx/dsp/afs_ladder.h"

#include <catch2/catch_test_macros.hpp>

using namespace afstest;
namespace k = engine::fx::dsp::afs;

#if APP_FX_HAVE_NEON
TEST_CASE("neon_vs_scalar_afs: each kernel equals its scalar reference bit for bit", "[afs][simd]")
{
    const auto inL = noise(16 * N, 41), inR = noise(16 * N, 42, 0.9f);
    k::HalfbandState a {}, b {};
    k::LadderState la {}, lb {};
    k::LadderCoefs cf {};
    for (int i = 0; i < k::kOver; ++i)
    {
        const float G = 0.05f + 0.6f * static_cast<float>(i) / k::kOver;
        const float fb = 4.4f * static_cast<float>(i % 7) / 6.0f;
        cf.g[i] = G;
        cf.g2[i] = G * G;
        cf.g3[i] = G * G * G;
        cf.inv[i] = 1.0f / (1.0f + fb * G * G * G * G);
        cf.kio[i] = fb * cf.inv[i] * (1.0f - G);
    }
    const auto nz = noise(2 * k::kOver, 43, 1e-6f);
    for (int blk = 0; blk < 16; ++blk)
    {
        const auto o = static_cast<std::size_t>(blk * N);
        alignas(16) float upA[4 * N], upB[4 * N], ladA[4 * N], ladB[4 * N];
        k::neon::upsample(a, inL.data() + o, inR.data() + o, upA);
        k::scalar::upsample(b, inL.data() + o, inR.data() + o, upB);
        REQUIRE(std::memcmp(upA, upB, sizeof upA) == 0);
        for (int i = 0; i < 4 * N; ++i)
            upA[i] = upB[i] = upA[i] * 3.0f; // drive the saturators
        k::neon::ladder(la, cf, upA, nz.data(), ladA, (blk & 1) != 0);
        k::scalar::ladder(lb, cf, upB, nz.data(), ladB, (blk & 1) != 0);
        REQUIRE(std::memcmp(ladA, ladB, sizeof ladA) == 0);
        REQUIRE(std::memcmp(&la, &lb, sizeof la) == 0);
        alignas(16) float dl[N], dr[N], el[N], er[N];
        k::neon::downsample(a, ladA, dl, dr);
        k::scalar::downsample(b, ladB, el, er);
        REQUIRE(std::memcmp(dl, el, sizeof dl) == 0);
        REQUIRE(std::memcmp(dr, er, sizeof dr) == 0);
    }
    for (double hz : {20.0, 300.0, 2000.0})
    {
        k::HighPass ha, hb;
        ha.setCutoff(hz, 44100.0);
        hb.setCutoff(hz, 44100.0);
        auto l1 = inL, r1 = inR, l2 = inL, r2 = inR;
        for (int n : {0, 1, 37, 64})
        {
            k::neon::highPass(ha, l1.data(), r1.data(), n);
            k::scalar::highPass(hb, l2.data(), r2.data(), n);
        }
        k::neon::highPass(ha, l1.data(), r1.data(), 16 * N);
        k::scalar::highPass(hb, l2.data(), r2.data(), 16 * N);
        CHECK(sameBits(l1, l2));
        CHECK(sameBits(r1, r2));
    }
}

TEST_CASE("neon_vs_scalar_afs: the whole model equals the scalar reference bit for bit", "[afs][simd]")
{
    const auto inL = noise(96 * N, 44, 1.2f), inR = sine(96 * N, 220.0, 0.8);
    for (double fs : {44100.0, 48000.0})
    {
        engine::afs::ModelProcessor a, b;
        a.prepare(fs, N);
        b.prepare(fs, N);
        b.useScalarKernels(true);
        engine::afs::AfsControl c;
        auto l1 = inL, r1 = inR, l2 = inL, r2 = inR;
        for (int blk = 0; blk < 96; ++blk)
        {
            c.lpCutoff01 = 0.2f + 0.7f * static_cast<float>(blk % 13) / 12.0f;
            c.resonance01 = static_cast<float>(blk % 11) / 10.0f;
            c.hpCutoff01 = (blk / 8) % 3 == 0 ? 0.0f : 0.4f;
            c.twoPole = (blk / 16) % 2 == 1;
            c.envAmount = static_cast<float>(blk % 5) - 2.0f;
            c.lfo = 0.3f * static_cast<float>((blk % 7) - 3);
            c.trig = blk % 9 == 0 ? static_cast<std::uint8_t>(1 + blk % 64) : 0;
            if (blk == 50)
            {
                a.reset();
                b.reset();
            }
            const auto o = static_cast<std::size_t>(blk * N);
            a.process(l1.data() + o, r1.data() + o, N, c);
            b.process(l2.data() + o, r2.data() + o, N, c);
        }
        CHECK(sameBits(l1, l2));
        CHECK(sameBits(r1, r2));
    }
}
#else
TEST_CASE("neon_vs_scalar_afs: each kernel equals its scalar reference bit for bit", "[afs][simd]")
{
    SKIP("no NEON on this host: scripts/neon-tests.sh runs it under qemu-arm (Cortex-A9)");
}
TEST_CASE("neon_vs_scalar_afs: the whole model equals the scalar reference bit for bit", "[afs][simd]")
{
    SKIP("no NEON on this host: scripts/neon-tests.sh runs it under qemu-arm (Cortex-A9)");
}
#endif
