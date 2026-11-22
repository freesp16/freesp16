// NEON ↔ scalar parity of LoFi and Distortion (bit-exact on normal inputs): the same
// effect run with its NEON kernels and with its scalar reference kernels (useScalarKernels) gives the same bits for
// every block, over settings that exercise every stage (preamp ramp and clip, crusher, S&H, every filter type and
// position, glides, gated jumps and type crossfades; drive, auto-gain, all three one-poles). On x86-64 hosts the
// NEON branch does not exist and the tests SKIP; scripts/neon-tests.sh runs them under qemu-arm as a Cortex-A9.
#include "tests/unit/fx_color/ColorTestUtil.h"

#include "engine/fx/dsp/Numerics.h"
#include "engine/fx/dsp/lofi_biquad.h"
#include "engine/fx/effects/Distortion.h"
#include "engine/fx/effects/LoFi.h"

#include <catch2/catch_test_macros.hpp>

using namespace engine::fx;
using namespace colortest;

#if APP_FX_HAVE_NEON
TEST_CASE("neon_vs_scalar_lofi: NEON kernels equal the scalar references bit for bit", "[fx_color][lofi][simd]")
{
    const auto inL = noise(48 * N, 21), inR = noise(48 * N, 22, 0.5f);
    struct Set
    {
        float k[6];
        int type, pos;
    };
    const Set sets[] = {{{0.3f, 0.7f, 0.5f, 0.45f, 0.4f, 1.0f}, 0, 0}, {{0.5f, 0.1f, 3.0f / 17, 0.6f, 0.2f, 1.0f}, 1, 1},
                        {{1.0f, 0.5f, 15.0f / 17, 0.3f, 0.9f, 1.0f}, 2, 0}, {{0.0f, 1.0f, 1.0f, 0.566f, 0.05f, 1.0f}, 3, 1},
                        {{0.2f, 0.9f, 16.0f / 17, 0.8f, 1.0f, 1.0f}, 1, 0}};
    for (const auto& s : sets)
        for (std::uint32_t q : {kQuirkProfileReference, kQuirkProfileSixteen})
        {
            LoFi a, b;
            b.useScalarKernels(true);
            for (auto* e : {&a, &b})
            {
                e->prepare(44100.0, N);
                for (int k = 0; k < 6; ++k)
                    e->setParam(k, s.k[k]);
                e->setOption(LoFi::kOptFilterType, s.type);
                e->setOption(LoFi::kOptPosition, s.pos);
                e->setQuirkProfile(q);
                e->reset();
            }
            CHECK(sameBits(runDirect(a, inL, inR, 44100.0), runDirect(b, inL, inR, 44100.0)));
            for (auto* e : {&a, &b}) // a gated cutoff jump, a glide, a type crossfade and a position change
            {
                e->setParam(3, 0.1f);
                e->setParam(4, 0.6f);
                e->setParam(0, 0.8f);
                e->setOption(LoFi::kOptFilterType, (s.type + 1) % 4);
                e->setOption(LoFi::kOptPosition, 1 - s.pos);
            }
            CHECK(sameBits(runDirect(a, inL, inR, 44100.0), runDirect(b, inL, inR, 44100.0)));
        }
}

TEST_CASE("neon_vs_scalar_lofi: the L/R-lane biquad equals the scalar biquad for every type", "[fx_color][lofi][simd]")
{
    using dsp::lofi::Biquad;
    using dsp::lofi::FilterKind;
    const auto x = noise(4 * N, 23), y = noise(4 * N, 24);
    for (auto kind : {FilterKind::HighPass, FilterKind::LowPass, FilterKind::BandPass})
        for (int n : {0, 1, 3, 44, 64})
        {
            Biquad a, b;
            a.init(kind, 44100);
            b.init(kind, 44100);
            a.setCoefs(1234.5f, 2.5);
            b.setCoefs(1234.5f, 2.5);
            auto l1 = x, r1 = y, l2 = x, r2 = y;
            a.processNeon(l1.data(), r1.data(), 2 * N);
            b.processScalar(l2.data(), r2.data(), 2 * N);
            a.setCoefs(300.0f, 0.7); // a type switch's re-prime of the last four inputs
            b.setCoefs(300.0f, 0.7);
            a.setPrime(true);
            b.setPrime(true);
            a.processNeon(l1.data() + 2 * N, r1.data() + 2 * N, n);
            b.processScalar(l2.data() + 2 * N, r2.data() + 2 * N, n);
            a.processNeon(l1.data() + 2 * N + n, r1.data() + 2 * N + n, N);
            b.processScalar(l2.data() + 2 * N + n, r2.data() + 2 * N + n, N);
            CHECK(sameBits(l1, l2));
            CHECK(sameBits(r1, r2));
        }
}

TEST_CASE("neon_vs_scalar_distortion: NEON kernels equal the scalar references bit for bit",
          "[fx_color][distortion][simd]")
{
    const auto inL = noise(64 * N, 25, 0.6f), inR = noise(64 * N, 26, 0.05f);
    const float sets[][6] = {{0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f},
                             {1.0f, 0.5f, 0.3f, 0.8f, 0.5f, 1.0f},
                             {0.5f, 0.2f, 0.6f, 0.4f, 0.1f, 0.4f},
                             {0.75f, 0.9f, 0.1f, 0.95f, 1.0f, 1.0f}};
    for (const auto& s : sets)
        for (std::uint32_t q : {kQuirkProfileReference, kQuirkProfileSixteen})
        {
            Distortion a, b;
            b.useScalarKernels(true);
            for (auto* e : {&a, &b})
            {
                e->prepare(48000.0, N);
                for (int k = 0; k < 6; ++k)
                    e->setParam(k, s[k]);
                e->setQuirkProfile(q);
                e->reset();
            }
            CHECK(sameBits(runDirect(a, inL, inR, 48000.0), runDirect(b, inL, inR, 48000.0)));
        }
}
#else
TEST_CASE("neon_vs_scalar_lofi: NEON kernels equal the scalar references bit for bit", "[fx_color][lofi][simd]")
{
    SKIP("no NEON on this host: scripts/neon-tests.sh runs it under qemu-arm (Cortex-A9)");
}
TEST_CASE("neon_vs_scalar_distortion: NEON kernels equal the scalar references bit for bit",
          "[fx_color][distortion][simd]")
{
    SKIP("no NEON on this host: scripts/neon-tests.sh runs it under qemu-arm (Cortex-A9)");
}
#endif

TEST_CASE("neon_vs_scalar_lofi: the scalar reference kernels equal the default path on this host",
          "[fx_color][lofi][simd]")
{
    // On a host without NEON both paths are the scalar reference: the switch itself must not change the output.
    const auto inL = noise(16 * N, 27), inR = noise(16 * N, 28);
    LoFi a, b;
    b.useScalarKernels(true);
    for (auto* e : {&a, &b})
    {
        e->prepare(44100.0, N);
        const float k[6] = {0.4f, 0.3f, 0.5f, 0.4f, 0.5f, 1.0f};
        for (int i = 0; i < 6; ++i)
            e->setParam(i, k[i]);
        e->setOption(LoFi::kOptFilterType, LoFi::kBandPass);
        e->reset();
    }
    CHECK(sameBits(runDirect(a, inL, inR, 44100.0), runDirect(b, inL, inR, 44100.0)));
}
