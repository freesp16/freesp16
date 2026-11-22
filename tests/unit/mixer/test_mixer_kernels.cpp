// The mixer's NEON kernels against their scalar references (MixerKernels.h: every NEON kernel has a scalar reference,
// compared on the host). Strip outputs and buses are bit-identical (same operations, same order, no FMA);
// meter peaks are exact, sums of squares agree to float rounding (different summation order).
#include "tests/unit/mixer/MixerTestUtil.h"

#include "engine/mixer/MixerKernels.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>

namespace k = engine::mixer::kernels;
using mixertest::kN;

namespace
{
struct Buffers
{
    alignas(16) float l[kN], r[kN], mainL[kN], mainR[kN], sendL[kN], sendR[kN], gains[4 * kN], mute[kN];
};

[[maybe_unused]] void fill(Buffers& b, mixertest::Noise& n)
{
    for (int i = 0; i < kN; ++i)
    {
        b.l[i] = n.next(0.9f);
        b.r[i] = n.next(0.9f);
        b.mainL[i] = n.next(0.5f);
        b.mainR[i] = n.next(0.5f);
        b.sendL[i] = n.next(0.5f);
        b.sendR[i] = n.next(0.5f);
        b.mute[i] = 0.5f + 0.5f * n.next();
    }
    for (int i = 0; i < 4 * kN; ++i)
        b.gains[i] = 1.0f + n.next();
}

[[maybe_unused]] bool same(const float* a, const float* b) { return std::memcmp(a, b, sizeof(float) * kN) == 0; }

[[maybe_unused]] void checkMeter(const k::StereoMeter& a, const k::StereoMeter& b)
{
    CHECK(a.peakL == b.peakL);
    CHECK(a.peakR == b.peakR);
    CHECK(a.sumL == Catch::Approx(b.sumL).epsilon(1e-5));
    CHECK(a.sumR == Catch::Approx(b.sumR).epsilon(1e-5));
}
} // namespace

TEST_CASE("mixer.kernels: the strip kernel's 8 variants match the scalar reference bit for bit", "[mixer][kernels]")
{
    if (! k::hasNeon())
        SKIP("no NEON on this host");
#if APP_MIXER_HAVE_NEON
    mixertest::Noise noise;
    for (int variant = 0; variant < 8; ++variant)
    {
        INFO("ramp " << (variant & 1) << " mute " << (variant >> 1 & 1) << " send " << (variant >> 2 & 1));
        auto a = std::make_unique<Buffers>(), b = std::make_unique<Buffers>();
        fill(*a, noise);
        *b = *a;
        auto args = [&](Buffers& x) {
            k::StripArgs s;
            s.l = x.l;
            s.r = x.r;
            s.mainL = x.mainL;
            s.mainR = x.mainR;
            s.sendL = variant & 4 ? x.sendL : nullptr;
            s.sendR = variant & 4 ? x.sendR : nullptr;
            s.mute = variant & 2 ? x.mute : nullptr;
            s.gains = x.gains;
            s.rampGains = (variant & 1) != 0;
            return s;
        };
        const auto ma = k::scalar::strip(args(*a), kN);
        const auto mb = k::neon::strip(args(*b), kN);
        CHECK(same(a->l, b->l));
        CHECK(same(a->r, b->r));
        CHECK(same(a->mainL, b->mainL));
        CHECK(same(a->mainR, b->mainR));
        CHECK(same(a->sendL, b->sendL));
        CHECK(same(a->sendR, b->sendR));
        checkMeter(ma, mb);
    }
#endif
}

TEST_CASE("mixer.kernels: unity sum, ramp fill and meters match the scalar reference", "[mixer][kernels]")
{
    if (! k::hasNeon())
        SKIP("no NEON on this host");
#if APP_MIXER_HAVE_NEON
    mixertest::Noise noise;
    auto a = std::make_unique<Buffers>(), b = std::make_unique<Buffers>();
    fill(*a, noise);
    *b = *a;
    checkMeter(k::scalar::unity(a->l, a->r, a->mainL, a->mainR, kN), k::neon::unity(b->l, b->r, b->mainL, b->mainR, kN));
    CHECK(same(a->mainL, b->mainL));
    CHECK(same(a->mainR, b->mainR));
    checkMeter(k::scalar::meter(a->l, a->r, kN), k::neon::meter(a->l, a->r, kN));

    // Ramp lanes: idle, ramping through the block, starting mid-block, ending mid-block, waiting past the block.
    k::Ramp4 s {};
    const std::array<std::array<int, 2>, 4> spans {{{0, 0}, {0, 65}, {20, 65}, {5, 37}}};
    for (int lane = 0; lane < 4; ++lane)
    {
        s.cur[lane] = 1.0f - 0.1f * static_cast<float>(lane);
        s.target[lane] = 0.3f;
        s.step[lane] = (s.target[lane] - s.cur[lane]) * engine::mixer::GainRamp::kInv;
        s.start[lane] = spans[static_cast<std::size_t>(lane)][0];
        s.end[lane] = spans[static_cast<std::size_t>(lane)][1];
    }
    s.step[0] = 0.0f;
    s.target[0] = s.cur[0];
    alignas(16) float outA[4 * kN], outB[4 * kN], accA[4], accB[4];
    k::scalar::rampFill(s, outA, accA, kN);
    k::neon::rampFill(s, outB, accB, kN);
    CHECK(std::memcmp(outA, outB, sizeof outA) == 0);
    CHECK(std::memcmp(accA, accB, sizeof accA) == 0);
    // the fast path (lanes idle or ramping through the whole block) gives the same values as the general one
    k::Ramp4 w = s;
    w.start[1] = w.start[2] = w.start[3] = 0;
    w.end[1] = w.end[2] = w.end[3] = kN + 1;
    alignas(16) float outC[4 * kN], outD[4 * kN], accC[4], accD[4];
    k::scalar::rampFill(w, outC, accC, kN);
    k::neon::rampFill(w, outD, accD, kN);
    CHECK(std::memcmp(outC, outD, sizeof outC) == 0);
    CHECK(std::memcmp(accC, accD, sizeof accC) == 0);
    // lane 3 ends at frame 37: from there it is exactly the target
    CHECK(outA[4 * 36 + 3] != 0.3f);
    CHECK(outA[4 * 37 + 3] == 0.3f);
    // lane 2 holds its value until frame 20, then steps
    CHECK(outA[4 * 20 + 2] == s.cur[2]);
    CHECK(outA[4 * 21 + 2] == s.cur[2] + s.step[2]);
#endif
}
