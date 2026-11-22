// Multi-mode filter (spec 02-features/effects/multimode-filter.md): parameter laws and kinds, glide step
// counts against the research model mmf.py (values computed with the pinned model, same call sequence), the quirk
// profile (quirk_filter: FX-580 start-up gate, FX-581 change-detection trap), and the NEON path against its scalar
// reference (neon_vs_scalar_filter: bit for bit; skipped on hosts without NEON — scripts/neon-tests.sh runs it under
// qemu-arm as a Cortex-A9). The goldens are tests/golden/filter{,_48k}.
#include "core/util/Realtime.h"
#include "engine/fx/EffectRegistry.h"
#include "engine/fx/FxShell.h"
#include "engine/fx/effects/MultiModeFilter.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <vector>

using engine::fx::effects::MultiModeFilter;
using Catch::Approx;

namespace
{
constexpr int N = engine::kBlockFrames;

engine::fx::ProcessContext ctx(double fs = 44100.0)
{
    engine::fx::ProcessContext c{fs, 120.0, false, 0u, {}, 0};
    c.triggerOffset.fill(-1);
    return c;
}

struct Signal
{
    std::vector<float> l, r;
};
Signal sine(int blocks, double hz = 1000.0, float a = 0.5f, double fs = 44100.0)
{
    Signal s;
    for (int i = 0; i < blocks * N; ++i)
    {
        s.l.push_back(a * static_cast<float>(std::sin(2.0 * 3.141592653589793 * hz * i / fs)));
        s.r.push_back(s.l.back());
    }
    return s;
}
Signal noise(int blocks, std::uint32_t seed, float a = 0.5f)
{
    Signal s;
    std::uint32_t x = seed;
    auto next = [&]
    {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        return a * (static_cast<float>(x >> 8) * (2.0f / 16777216.0f) - 1.0f);
    };
    for (int i = 0; i < blocks * N; ++i)
    {
        s.l.push_back(next());
        s.r.push_back(next());
    }
    return s;
}

void defaults(MultiModeFilter& f)
{
    for (int k = 0; k < 6; ++k)
        f.setParam(k, MultiModeFilter::kDefaults[k]);
    f.setOption(MultiModeFilter::kOptType, MultiModeFilter::kLowPass);
    f.setOption(MultiModeFilter::kOptSlope, MultiModeFilter::k12dB);
}

// one block in place (process or the scalar reference)
void block(MultiModeFilter& f, float* l, float* r, bool scalar = false)
{
    const auto c = ctx();
    if (scalar)
        f.processScalar(l, r, c);
    else
        f.process(l, r, c);
}

Signal run(MultiModeFilter& f, const Signal& in, bool scalar = false)
{
    Signal out = in;
    for (std::size_t b = 0; b < in.l.size() / N; ++b)
        block(f, out.l.data() + b * N, out.r.data() + b * N, scalar);
    return out;
}

double blockRms(const std::vector<float>& v, int b)
{
    double s = 0;
    for (int i = 0; i < N; ++i)
    {
        const auto x = static_cast<double>(v[static_cast<std::size_t>(b * N + i)]);
        s += x * x;
    }
    return std::sqrt(s / N);
}

bool sameBits(const std::vector<float>& a, const std::vector<float>& b, std::size_t from = 0)
{
    for (std::size_t i = from; i < a.size(); ++i)
        if (std::memcmp(&a[i], &b[i], sizeof(float)) != 0)
            return false;
    return true;
}
} // namespace

TEST_CASE("fx.filter: parameter laws, kinds and the descriptor (FX-520/521/523, FX-582)", "[fx][filter]")
{
    CHECK(MultiModeFilter::cutoffOf(0.0f) == 20.0f);
    CHECK(MultiModeFilter::cutoffOf(1.0f) == 20000.0f);
    CHECK(MultiModeFilter::cutoffOf(0.566f) == Approx(997.77).epsilon(1e-4));
    CHECK(MultiModeFilter::qOf(5.0f / 98.0f) == 0.7f);
    CHECK(MultiModeFilter::qOf(1.0f) == 10.0f);
    CHECK(MultiModeFilter::gainOf(2.0f / 3.0f) == 1.0f);
    CHECK(MultiModeFilter::gainOf(0.0f) == Approx(0.0316228).epsilon(1e-6));
    CHECK(MultiModeFilter::gainOf(1.0f) == Approx(5.623413).epsilon(1e-6));
    CHECK(MultiModeFilter::glideOf(0.0f) == 64.0f);
    CHECK(MultiModeFilter::glideOf(0.5f) == 320.0f);
    CHECK(MultiModeFilter::glideOf(1.0f) == 576.0f);
    // FILTERTYPE × FILTERDB → kind (0x107c24); the app enum maps LP/HP/BP/Off and 12/24 dB (FX-582)
    CHECK(MultiModeFilter::kindOf(0, 1) == 0);
    CHECK(MultiModeFilter::kindOf(2, 1) == 2);
    CHECK(MultiModeFilter::kindOf(1, 0) == 4);
    CHECK(MultiModeFilter::kindOf(1, 2) == 4);
    CHECK(MultiModeFilter::kindOf(6, 1) == 6);
    CHECK(MultiModeFilter::kindOf(3, 1) == 6); // "LowPass24" = no filter
    const int types[] = {MultiModeFilter::kLowPass, MultiModeFilter::kHighPass, MultiModeFilter::kBandPass};
    for (int slope = 0; slope < 2; ++slope)
        for (int t = 0; t < 3; ++t)
        {
            MultiModeFilter f;
            f.prepare(44100.0, N);
            defaults(f);
            f.setOption(MultiModeFilter::kOptType, types[t]);
            f.setOption(MultiModeFilter::kOptSlope, slope);
            f.reset();
            CHECK(f.pendingKind() == t + 3 * slope);
            auto y = noise(1, 2);
            run(f, y);
            CHECK(f.kind() == t + 3 * slope);
        }
    MultiModeFilter off;
    off.prepare(48000.0, N);
    defaults(off);
    off.setOption(MultiModeFilter::kOptType, MultiModeFilter::kOff);
    off.reset();
    auto in = noise(4, 3);
    auto out = run(off, in);
    CHECK(off.kind() == MultiModeFilter::kKindOff);
    CHECK(sameBits(in.l, out.l)); // Off at 0 dB: the input unchanged (Sixteen: no start-up gate)
    CHECK(sameBits(in.r, out.r));
    // 24 dB = two identical biquads at √Q (FX-520): same coefficients in both stages
    MultiModeFilter f;
    f.prepare(44100.0, N);
    defaults(f);
    f.setOption(MultiModeFilter::kOptSlope, MultiModeFilter::k24dB);
    f.setParam(2, 0.5f);
    f.reset();
    auto x = noise(2, 5);
    run(f, x);
    CHECK(std::memcmp(&f.coefs(0), &f.coefs(1), sizeof(engine::fx::dsp::filt::Coefs)) == 0);
    CHECK(f.q() == MultiModeFilter::qOf(0.5f));

    const auto* d = engine::fx::EffectRegistry::builtin().find("filter");
    REQUIRE(d != nullptr);
    CHECK(d->params[4].empty()); // k4 unused (FX-062)
    CHECK(d->shell.wetHighPass);
    CHECK(d->shell.mixLaw == static_cast<std::uint8_t>(engine::fx::MixLaw::CrossfadeK5));
    CHECK(d->options.size() == 2);
}

TEST_CASE("fx.filter: glide step counts equal the research model exactly (FX-521/522)", "[fx][filter]")
{
    // (glide, cutoff from, cutoff to, Q to) → after one block: cutoff steps, Q steps, gate fading (state 1), cutoff
    // — mmf.py at 44.1 kHz: insert defaults, reset, 4 silent blocks, the change, one silent block.
    struct Row
    {
        float glide, from, to, qTo;
        int cutSteps, qSteps;
        bool gated;
        float cutoff;
    };
    const Row rows[] = {
        {0.0f, 0.566f, 0.66f, -1, 6, 0, false, 1111.4884033203125f},
        {0.0f, 0.566f, 0.8f, -1, 16, 1, true, 997.7688598632812f},
        {0.5f, 0.566f, 0.1f, -1, 149, 0, false, 976.4611206054688f},
        {1.0f, 0.2f, 0.9f, 0.5f, 403, 51, false, 80.5820541381836f},
        {0.25f, 0.7f, 0.69f, 1.0f, 2, 68, false, 2428.8740234375f},
        {0.0f, 0.3f, 0.3001f, 0.0f, 0, 32, false, 158.9754180908203f},
    };
    float z[N] = {}, zr[N] = {};
    for (const auto& row : rows)
    {
        INFO("glide " << row.glide << " " << row.from << " -> " << row.to);
        MultiModeFilter f;
        f.prepare(44100.0, N);
        f.setQuirkProfile(engine::fx::kQuirkProfileReference);
        defaults(f);
        f.setParam(3, row.glide);
        f.setParam(0, row.from);
        f.reset();
        for (int b = 0; b < 4; ++b)
            block(f, z, zr);
        f.setParam(0, row.to);
        if (row.qTo >= 0)
            f.setParam(2, row.qTo);
        block(f, z, zr);
        CHECK(f.cutoffSteps() == row.cutSteps);
        CHECK(f.qSteps() == row.qSteps);
        CHECK(f.cutoffHz() == row.cutoff);
        CHECK(f.kind() == 0);
        (void) row.gated; // the gated jump is visible in the goldens (jump_above) and below
    }
}

TEST_CASE("fx.filter: quirk_filter start-up gate (FX-580) and change-detection trap (FX-581)", "[fx][filter][quirk]")
{
    // FX-580: at the defaults on a 1 kHz sine the reference's first four blocks are 0.183, 0.491, 0.700, 0.714 RMS gain;
    // Sixteen starts open; from sample 132 (block 3 on) both are bit-equal.
    const auto in = sine(12);
    MultiModeFilter ref, six;
    for (auto* f : {&ref, &six})
        f->prepare(44100.0, N);
    ref.setQuirkProfile(engine::fx::kQuirkProfileReference);
    six.setQuirkProfile(engine::fx::kQuirkProfileSixteen);
    for (auto* f : {&ref, &six})
    {
        defaults(*f);
        f->reset();
    }
    const auto a = run(ref, in), b = run(six, in);
    const double expect[4] = {0.183, 0.491, 0.700, 0.714};
    for (int k = 0; k < 4; ++k)
        CHECK(blockRms(a.l, k) / blockRms(in.l, k) == Approx(expect[k]).margin(0.0015));
    CHECK(blockRms(b.l, 0) / blockRms(in.l, 0) > 0.6); // no fade-in
    CHECK(sameBits(a.l, b.l, 3 * N));
    CHECK(sameBits(a.r, b.r, 3 * N));
    CHECK_FALSE(sameBits(a.l, b.l, 0));

    // FX-581: Off set before the first block (and nothing else). Reference: equal to the constructor's "last" value 6,
    // swallowed — the LP 12 dB the container was built as keeps running. Sixteen: applied (FX-056). (With the insert
    // defaults also sent, the slope change re-evaluates the kind and Off applies in both: golden filter/off_defaults.)
    const auto nin = noise(4, 11);
    MultiModeFilter rt, st;
    for (auto* f : {&rt, &st})
        f->prepare(44100.0, N);
    rt.setQuirkProfile(engine::fx::kQuirkProfileReference);
    st.setQuirkProfile(engine::fx::kQuirkProfileSixteen);
    for (auto* f : {&rt, &st})
    {
        f->setOption(MultiModeFilter::kOptType, MultiModeFilter::kOff);
        f->reset();
    }
    const auto ro = run(rt, nin), so = run(st, nin);
    CHECK(rt.kind() == 0);
    CHECK(st.kind() == MultiModeFilter::kKindOff);
    CHECK(sameBits(so.l, nin.l));
    CHECK_FALSE(sameBits(ro.l, nin.l));

    // The Sixteen profile also applies a first value equal to a constructor value (CUTOFF 0.5 = 632 Hz, not 1000 Hz).
    MultiModeFilter rc, sc;
    for (auto* f : {&rc, &sc})
        f->prepare(44100.0, N);
    rc.setQuirkProfile(engine::fx::kQuirkProfileReference);
    sc.setQuirkProfile(engine::fx::kQuirkProfileSixteen);
    for (auto* f : {&rc, &sc})
    {
        f->setParam(0, 0.5f);
        f->reset();
        run(*f, nin);
    }
    CHECK(rc.cutoffTarget() == 1000.0f);
    CHECK(sc.cutoffTarget() == MultiModeFilter::cutoffOf(0.5f));
}

TEST_CASE("fx.filter: reset applies pending changes and keeps the glide (FX-560); kClearState = fresh", "[fx][filter]")
{
    MultiModeFilter f;
    f.prepare(44100.0, N);
    defaults(f);
    f.setParam(3, 1.0f); // slow glide
    f.reset();
    auto in = noise(8, 21);
    run(f, in);
    f.setParam(0, 0.1f);
    float l[N] = {}, r[N] = {};
    block(f, l, r);
    const int steps = f.cutoffSteps();
    REQUIRE(steps > 10);
    f.reset(); // activation: the glide continues
    block(f, l, r);
    CHECK(f.cutoffSteps() < steps); // one or two steps per block (44.1-sample grid)
    CHECK(f.cutoffSteps() >= steps - 2);
    f.setParam(1, 1.0f);
    f.reset(); // pending gain applied at once by reset (ramp starts)
    CHECK(f.gain() == 1.0f);
    block(f, l, r);
    CHECK(f.gain() > 1.0f);

    MultiModeFilter fresh;
    fresh.prepare(44100.0, N);
    f.setOption(engine::fx::kClearState, 1);
    for (auto* e : {&f, &fresh})
    {
        defaults(*e);
        e->reset();
    }
    const auto a = run(f, in), b = run(fresh, in);
    CHECK(sameBits(a.l, b.l));
    CHECK(sameBits(a.r, b.r));
}

TEST_CASE("fx.filter: neon_vs_scalar_filter — the NEON path equals the scalar reference bit for bit", "[fx][filter][simd]")
{
#if ! APP_FX_HAVE_NEON
    SKIP("no NEON on this host (scripts/neon-tests.sh runs this under qemu-arm)");
#else
    core::rt::enableFlushToZero(); // the app's audio-thread numerics (FX-055): VFP flushes like NEON
    // every kind, glides, gated jumps, gain ramps, kind switches: a seeded walk over 200 blocks, both profiles
    for (std::uint32_t profile : {engine::fx::kQuirkProfileReference, engine::fx::kQuirkProfileSixteen})
        for (std::uint32_t seed : {1u, 77u, 4242u})
        {
            MultiModeFilter a, b;
            for (auto* f : {&a, &b})
            {
                f->prepare(44100.0, N);
                f->setQuirkProfile(profile);
                defaults(*f);
                f->reset();
            }
            const auto in = noise(200, seed * 3 + 1, 0.8f);
            auto oa = in, ob = in;
            std::uint32_t x = seed;
            auto draw = [&]
            {
                x ^= x << 13;
                x ^= x >> 17;
                x ^= x << 5;
                return x;
            };
            for (int blk = 0; blk < 200; ++blk)
            {
                if (draw() % 3 == 0)
                {
                    const std::uint32_t w = draw();
                    const float v = static_cast<float>(w >> 8) * (1.0f / 16777216.0f);
                    const int what = static_cast<int>(w % 6);
                    for (auto* f : {&a, &b})
                    {
                        if (what < 4)
                            f->setParam(what, v);
                        else if (what == 4)
                            f->setOption(MultiModeFilter::kOptType, static_cast<int>(w >> 3) % 4);
                        else
                            f->setOption(MultiModeFilter::kOptSlope, static_cast<int>(w >> 3) % 2);
                    }
                }
                block(a, oa.l.data() + blk * N, oa.r.data() + blk * N, false);
                block(b, ob.l.data() + blk * N, ob.r.data() + blk * N, true);
            }
            CHECK(sameBits(oa.l, ob.l));
            CHECK(sameBits(oa.r, ob.r));
        }
#endif
}
