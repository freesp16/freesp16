// LoFi (spec lofi.md): the quirk profile (FX-980 start-up gates, FX-981 drop-out after activation)
// shows exactly the documented differences, the stage laws (FX-920…922), reset (FX-960) and the registration
// (descriptor, ParamSpecs, defaults FX-961). The bit-exact comparison with lofi.py is the golden set tests/golden/lofi;
// the NEON ↔ scalar parity is test_lofi_neon.cpp.
#include "tests/unit/fx_color/ColorTestUtil.h"

#include "core/params/ParamRegistry.h"
#include "core/params/catalogue/fx.h"
#include "engine/fx/EffectRegistry.h"
#include "engine/fx/effects/LoFi.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace engine::fx;
using namespace colortest;

namespace
{
// The app defaults (FX-961), as the shell sends them (FX-056).
void defaults(EffectBase& e)
{
    const float d[6] = {0.0f, 1.0f, 1.0f, 0.566f, 5.0f / 98.0f, 1.0f};
    for (int k = 0; k < 6; ++k)
        e.setParam(k, d[k]);
}
} // namespace

TEST_CASE("quirk_lofi: FX-980 the reference's closed start-up gates fade the first 132 samples in", "[fx_color][lofi]")
{
    const auto inL = noise(8 * N, 3), inR = noise(8 * N, 4);
    LoFi ref, six;
    for (auto* e : {&ref, &six})
    {
        e->prepare(44100.0, N);
        defaults(*e);
    }
    ref.setQuirkProfile(kLoFiQuirkClosedGates); // only FX-980
    six.setQuirkProfile(0);
    ref.reset();
    six.reset();
    const auto a = leftOf(runDirect(ref, inL, inR, 44100.0)), b = leftOf(runDirect(six, inL, inR, 44100.0));
    // Sixteen: no start-up fade — the defaults are transparent (preamp scale f32(f32(1.2)·0.8333333) = 1).
    CHECK(b[0] == inL[0]);
    // Reference: x·(n/132)² (position gate × filter gate, each an accumulated 132-sample ramp), then equal.
    float g = 0.0f;
    const float step = 1.0f / 132.0f;
    for (int n = 0; n < 132; ++n)
    {
        INFO("sample " << n);
        CHECK(std::fabs(a[n] - b[n] * g * g) <= 1e-6f);
        g += step;
    }
    CHECK(a[0] == 0.0f);
    bool equalAfter = true;
    for (std::size_t n = 132; n < a.size(); ++n)
        equalAfter = equalAfter && a[n] == b[n];
    CHECK(equalAfter);
    // Once per lifetime: a second activation (reset) has no fade.
    ref.reset();
    const auto c = leftOf(runDirect(ref, inL, inR, 44100.0));
    CHECK(c[0] == b[0]);
}

TEST_CASE("quirk_lofi: FX-981 the drop-out after activation (Reference) is gone in the Sixteen profile",
          "[fx_color][lofi]")
{
    // Through the shell with Mix 1: exact digital silence for ⌊step⌋ − 147 samples starting at sample 147 (spec table).
    struct Row
    {
        float v;
        int silent44, silent48;
    };
    const Row rows[] = {{0.2f, 103, -1}, {0.1f, 354, 385}, {0.05f, 564, -1}, {0.0f, 855, 930}};
    for (const auto& row : rows)
        for (double fs : {44100.0, 48000.0})
        {
            const int expected = fs == 44100.0 ? row.silent44 : row.silent48;
            if (expected < 0)
                continue;
            INFO("reduction " << row.v << " fs " << fs);
            const int fade = FxShell::shellFadeSamples(fs);
            const auto inL = noise(32 * N, 5), inR = noise(32 * N, 6);
            for (std::uint32_t profile : {kQuirkProfileReference, kQuirkProfileSixteen})
            {
                LoFi e;
                ShellRig rig(e, fs, profile);
                const float d[6] = {0.0f, row.v, 1.0f, 0.566f, 5.0f / 98.0f, 1.0f};
                for (int k = 0; k < 6; ++k)
                    rig.shell.setParam(k, d[k]);
                rig.shell.activate(true);
                const auto y = rig.run(inL, inR);
                int run = 0;
                while (fade + run < static_cast<int>(y.size()) && y[static_cast<std::size_t>(fade + run)] == 0.0f)
                    ++run;
                if (profile == kQuirkProfileReference)
                {
                    CHECK(run == expected);
                    CHECK(static_cast<int>(e.holdStep()) - fade == expected);
                }
                else
                    CHECK(run == 0); // the first input sample is taken at once (phase 0)
            }
        }
}

TEST_CASE("lofi: Sixteen start-up applies the first parameter set at once (no type or glide fade)", "[fx_color][lofi]")
{
    const auto inL = noise(16 * N, 7), inR = noise(16 * N, 8);
    LoFi a, b;
    for (auto* e : {&a, &b})
    {
        e->prepare(44100.0, N);
        defaults(*e);
        e->setParam(3, 0.9f); // 12.6 kHz: from the constructor's 1 kHz the reference would gate-jump
        e->setOption(LoFi::kOptFilterType, LoFi::kLowPass);
        e->setOption(LoFi::kOptPosition, 0);
    }
    a.setQuirkProfile(kQuirkProfileSixteen);
    a.reset();
    CHECK(a.filterType() == LoFi::kLowPass);
    CHECK(a.position() == 0);
    const auto y = leftOf(runDirect(a, inL, inR, 44100.0));
    // The LP at 12.6 kHz passes most of the noise from the first block on: no 132-sample gate dip.
    double e0 = 0.0, e1 = 0.0;
    for (int n = 0; n < 132; ++n)
    {
        e0 += static_cast<double>(y[n]) * static_cast<double>(y[n]);
        e1 += static_cast<double>(y[n + 512]) * static_cast<double>(y[n + 512]);
    }
    CHECK(e0 > 0.5 * e1);
    b.setQuirkProfile(kQuirkProfileReference);
    b.reset();
    const auto z = leftOf(runDirect(b, inL, inR, 44100.0));
    double r0 = 0.0;
    for (int n = 0; n < 132; ++n)
        r0 += static_cast<double>(z[n]) * static_cast<double>(z[n]);
    CHECK(r0 < 0.5 * e1); // the reference fades in (FX-980)
}

TEST_CASE("lofi: preamp clip, square-root crusher and sample-and-hold laws (FX-920…922)", "[fx_color][lofi]")
{
    LoFi e;
    e.prepare(44100.0, N);
    defaults(e);
    e.setParam(2, 3.0f / 17.0f); // 4 bits
    e.reset();
    alignas(16) float l[N], r[N];
    for (int i = 0; i < N; ++i)
    {
        l[i] = -1.0f + 2.0f * static_cast<float>(i) / (N - 1);
        r[i] = 0.25f;
    }
    const auto ctx = context(44100.0);
    e.process(l, r, ctx);
    for (int i = 0; i < N; ++i)
    {
        const float x = -1.0f + 2.0f * static_cast<float>(i) / (N - 1);
        const float p = x; // the 0 dB preamp scale is exactly 1
        const float q = std::floor(16.0f * std::sqrt(std::fabs(p))) / 16.0f;
        CHECK(l[i] == (p > 0.0f ? 1.0f : -1.0f) * q * q);
    }
    CHECK(r[0] == 0.25f); // √0.25 = 0.5 = 8/16: on a step
    // Reduction 0.5: rate round(44.1·1000^0.5) = 1,395 Hz, hold step 44100/1395 (spec: "REDUCTION 0.5 gives 1,395 Hz")
    LoFi h;
    h.prepare(44100.0, N);
    defaults(h);
    h.setParam(1, 0.5f);
    h.reset();
    CHECK(h.holdStep() == 44100.0f / 1395.0f);
    for (int i = 0; i < N; ++i)
        l[i] = r[i] = static_cast<float>(i) / 64.0f;
    h.process(l, r, ctx);
    // Phase 0 (Sixteen): sample 0 is taken (φ → 31.61 − 1), held while φ >= 1, then sample 31 (φ 0.61), then 63.
    CHECK(l[0] == 0.0f);
    CHECK(l[30] == 0.0f);
    CHECK(l[31] == 31.0f / 64.0f);
    CHECK(l[62] == 31.0f / 64.0f);
    CHECK(l[63] == 63.0f / 64.0f);
    // Preamp 1 (42 dB): the clip ceiling (1 + 0.2g)/(1.2g) ≈ 0.173.
    LoFi p;
    p.prepare(44100.0, N);
    defaults(p);
    p.setParam(0, 1.0f);
    p.reset();
    std::vector<float> loud(4 * N, 0.9f);
    const auto y = leftOf(runDirect(p, loud, loud, 44100.0));
    CHECK(std::fabs(y.back() - 0.1733f) < 1e-3f);
}

TEST_CASE("lofi: reset re-applies pending changes and zeroes only the current filter (FX-960)", "[fx_color][lofi]")
{
    const auto inL = noise(8 * N, 9), inR = noise(8 * N, 10);
    LoFi a, b;
    for (auto* e : {&a, &b})
    {
        e->prepare(44100.0, N);
        defaults(*e);
        e->setOption(LoFi::kOptFilterType, LoFi::kBandPass);
        e->reset();
    }
    runDirect(a, inL, inR, 44100.0);
    a.reset(); // a second activation: taps of the BP zeroed, hold values 0
    b.setOption(kClearState, 1);
    defaults(b);
    b.setOption(LoFi::kOptFilterType, LoFi::kBandPass);
    b.reset();
    // a's glide/gate state is settled, b is fresh: both produce the same block after the reset.
    const auto ya = runDirect(a, inL, inR, 44100.0), yb = runDirect(b, inL, inR, 44100.0);
    CHECK(sameBits(ya, yb));
}

TEST_CASE("lofi: registration — descriptor, ParamSpecs and defaults (FX-961)", "[fx_color][lofi]")
{
    const auto* d = EffectRegistry::builtin().find("lofi");
    REQUIRE(d != nullptr);
    CHECK(d->name == "LO-FI");
    CHECK(d->shell.wetHighPass);
    CHECK(d->shell.mixLaw == static_cast<std::uint8_t>(MixLaw::CrossfadeK5));
    CHECK(d->options.size() == 2);
    const auto& reg = core::builtinParams();
    for (const char* slot : {"fx1", "fx2", "send", "master"})
    {
        const std::string s = slot;
        REQUIRE(reg.find(s + ".lofi.preamp") != nullptr);
        CHECK(reg.find(s + ".lofi.preamp")->defaultValue == 0.0f);
        CHECK(reg.find(s + ".lofi.reduction")->defaultValue == 1.0f);
        CHECK(reg.find(s + ".lofi.bits")->defaultValue == 1.0f);
        CHECK(reg.find(s + ".lofi.cutoff")->defaultValue == 0.566f);
        CHECK(reg.find(s + ".lofi.resonance")->defaultValue == 5.0f / 98.0f);
        CHECK(reg.find(s + ".lofi.mix")->defaultValue == 1.0f);
        CHECK(reg.find(s + ".lofi.type1")->defaultValue == 3.0f); // Off
        CHECK(reg.find(s + ".lofi.type2")->defaultValue == 1.0f); // Post
    }
    char buf[32];
    const auto* bits = reg.find("fx1.lofi.bits");
    REQUIRE(bits->format != nullptr);
    bits->format(1.0f, buf, sizeof buf);
    CHECK(std::string(buf) == "OFF");
    bits->format(0.5f, buf, sizeof buf);
    CHECK(std::string(buf) == "9 bit");
    reg.find("fx1.lofi.reduction")->format(0.5f, buf, sizeof buf);
    CHECK(std::string(buf) == "1.4 kHz");
}
