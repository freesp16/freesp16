// NEON ↔ scalar parity of Chorus, Flanger and Phaser (bit-exact on normal inputs): the same
// effect run with its NEON kernels and with its scalar reference kernels (useScalarKernels) gives the same bits for
// every block, over settings that exercise every path (Chorus: the ring wrap, negative read positions, the start-up
// mute ramp, the widest sweep; Flanger: feedback 0.99, the 48-sample knee, the ring wrap; Phaser: the 192-sample
// ramps, feedback, every corner) in both quirk profiles and at 44.1/48/96 kHz. On x86-64 hosts the NEON branch does
// not exist and the tests SKIP; scripts/neon-tests.sh runs them under qemu-arm as a Cortex-A9.
#include "tests/unit/fx_mod/ModTestUtil.h"

#include "engine/fx/FxShell.h"
#include "engine/fx/dsp/Numerics.h"
#include "engine/fx/effects/Chorus.h"
#include "engine/fx/effects/Flanger.h"
#include "engine/fx/effects/Phaser.h"

#include <catch2/catch_test_macros.hpp>

using namespace engine::fx;
using namespace modtest;

namespace
{
struct Set
{
    float k[6];
    int sync;
};

template <typename E>
void compareKernels(const Set* sets, int nSets)
{
    const auto inL = noise(160 * N, 31), inR = noise(160 * N, 32, 0.5f);
    for (int s = 0; s < nSets; ++s)
        for (double fs : {44100.0, 48000.0, 96000.0})
            for (std::uint32_t q : {kQuirkProfileReference, kQuirkProfileSixteen})
            {
                INFO("set " << s << " fs " << fs << " profile " << q);
                E a, b;
                b.useScalarKernels(true);
                for (auto* e : {&a, &b})
                {
                    e->prepare(fs, N);
                    for (int k = 0; k < 6; ++k)
                        e->setParam(k, sets[s].k[k]);
                    e->setOption(0, sets[s].sync);
                    e->setQuirkProfile(q);
                    e->reset();
                }
                auto events = [&](E& e)
                {
                    return [&e](int blk, ProcessContext& ctx)
                    {
                        if (blk == 50) // mid-run changes: rate, width, phase, k3, k4, a retrigger, a tempo change
                        {
                            e.setParam(0, 0.83f);
                            e.setParam(1, 0.37f);
                            e.setParam(2, 0.61f);
                            e.setParam(3, 0.12f);
                            e.setParam(4, 0.95f);
                            e.retrigger();
                            ctx.bpm = 147.0;
                        }
                        if (blk == 100)
                        {
                            e.reset();
                            e.setParam(3, 1.0f);
                        }
                    };
                };
                const auto ya = runDirect(a, inL, inR, fs, 120.0, events(a));
                const auto yb = runDirect(b, inL, inR, fs, 120.0, events(b));
                CHECK(sameBits(ya, yb));
            }
}
} // namespace

#if APP_FX_HAVE_NEON
TEST_CASE("neon_vs_scalar_chorus: NEON taps and interpolation equal the scalar reference bit for bit",
          "[fx_mod][chorus][simd]")
{
    const Set sets[] = {{{19.0f / 99, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f}, 0},     // defaults (ring wrap at block 69)
                        {{1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 1.0f}, 0},           // 10 Hz, the widest sweep, 180°
                        {{7.0f / 9, 0.5f, 0.3f, 0.6f, 0.0f, 1.0f}, 1},       // SYNC 1/4 beat
                        {{0.02f, 0.01f, 0.9f, 0.0f, 0.0f, 1.0f}, 0}};        // shallow, slow
    compareKernels<Chorus>(sets, 4);
}

TEST_CASE("neon_vs_scalar_flanger: the L/R-lane feedback loop and the vector gains equal the scalar reference",
          "[fx_mod][flanger][simd]")
{
    const Set sets[] = {{{19.0f / 99, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f}, 0},
                        {{0.5f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f}, 0},           // 7 ms, feedback 0.99, 180°
                        {{0.9f, 1.0f, 0.2f, 14.0f / 69, 0.6f, 1.0f}, 0},     // straddling the knee
                        {{1.0f, 0.7f, 0.5f, 0.5f, 0.8f, 1.0f}, 1}};          // SYNC 1 beat
    compareKernels<Flanger>(sets, 4);
}

TEST_CASE("neon_vs_scalar_phaser: the L/R-lane all-pass chain equals the scalar reference bit for bit",
          "[fx_mod][phaser][simd]")
{
    const Set sets[] = {{{199.0f / 999, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f}, 0},   // defaults
                        {{0.05f, 0.5f, 1.0f, 0.8f, 1.0f, 1.0f}, 0},          // feedback 0.99, 180°
                        {{0.0f, 0.0f, 0.3f, 204.0f / 256, 0.4f, 1.0f}, 0},   // width 0
                        {{5.0f / 11, 0.8f, 0.6f, 0.3f, 0.7f, 1.0f}, 1}};     // SYNC
    compareKernels<Phaser>(sets, 4);
}
#else
TEST_CASE("neon_vs_scalar_chorus: NEON taps and interpolation equal the scalar reference bit for bit",
          "[fx_mod][chorus][simd]")
{
    SKIP("no NEON on this host: scripts/neon-tests.sh runs it under qemu-arm (Cortex-A9)");
}

TEST_CASE("neon_vs_scalar_flanger: the L/R-lane feedback loop and the vector gains equal the scalar reference",
          "[fx_mod][flanger][simd]")
{
    SKIP("no NEON on this host: scripts/neon-tests.sh runs it under qemu-arm (Cortex-A9)");
}

TEST_CASE("neon_vs_scalar_phaser: the L/R-lane all-pass chain equals the scalar reference bit for bit",
          "[fx_mod][phaser][simd]")
{
    SKIP("no NEON on this host: scripts/neon-tests.sh runs it under qemu-arm (Cortex-A9)");
}
#endif

TEST_CASE("fx.mod: the scalar test hook equals the default path on this host (Chorus, Flanger, Phaser)",
          "[fx_mod][simd]")
{
    // Without NEON the default path is the scalar one; with NEON this is the same check as above. Either way the
    // test hook must not change the result.
    const Set chorus[] = {{{1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 1.0f}, 0}};
    const Set flanger[] = {{{0.5f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f}, 0}};
    const Set phaser[] = {{{0.05f, 0.5f, 1.0f, 0.8f, 1.0f, 1.0f}, 0}};
    compareKernels<Chorus>(chorus, 1);
    compareKernels<Flanger>(flanger, 1);
    compareKernels<Phaser>(phaser, 1);
}
