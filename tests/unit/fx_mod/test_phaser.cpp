// Phaser behaviour and quirks (spec phaser.md FX-320…325, FX-360…362, FX-380). The bit-exact comparison
// with the research model is tests/golden/phaser{,_48k} (Reference profile, parity maths); these tests pin the quirk
// flag, the corners, the ramps, the notch positions, the lifecycle, the maths switch and the registration.
#include "tests/unit/fx_mod/ModTestUtil.h"

#include "core/params/ParamRegistry.h"
#include "engine/fx/EffectRegistryOrder.h"
#include "engine/fx/FxShell.h"
#include "engine/fx/dsp/phaser_tanf.h"
#include "engine/fx/effects/Phaser.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <complex>
#include <string>
#include <vector>

using namespace engine::fx;
using namespace modtest;

namespace
{
void setup(Phaser& p, double fs, std::uint32_t quirks, const float (&k)[6], int sync = 0, bool parity = false)
{
    p.prepare(fs, N);
    p.setOption(Phaser::kOptParityMaths, parity ? 1 : 0);
    for (int i = 0; i < 6; ++i)
        p.setParam(i, k[i]);
    p.setOption(Phaser::kOptSync, sync);
    p.setQuirkProfile(quirks);
    p.reset();
}

void run(Phaser& p, int blocks, double fs = 44100.0, double bpm = 120.0)
{
    const auto ctx = context(fs, bpm);
    alignas(16) float l[N]{}, r[N]{};
    for (int b = 0; b < blocks; ++b)
        p.process(l, r, ctx);
}

constexpr float kDefaults[6] = {199.0f / 999.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f};
} // namespace

TEST_CASE("quirk_phaser: FX-380 the LFO period from a literal 44,100 Hz (Reference) or fs (Sixteen)",
          "[fx_mod][phaser]")
{
    for (double fs : {44100.0, 48000.0})
    {
        Phaser ref, six;
        setup(ref, fs, kQuirkProfileReference, kDefaults);
        setup(six, fs, kQuirkProfileSixteen, kDefaults);
        // activation initialises the LFO at fs in both (2 Hz: P = fs/2 − 1) ...
        CHECK(ref.oscillator().period() == static_cast<float>(fs) / 2.0f - 1.0f);
        CHECK(six.oscillator().period() == static_cast<float>(fs) / 2.0f - 1.0f);
        // ... the first Rate apply (period 500 ms through setPeriodMs) uses the literal 44,100 in the Reference
        run(ref, 1, fs);
        run(six, 1, fs);
        CHECK(ref.periodMs() == 500.0f);
        CHECK(ref.oscillator().period() == 44100.0f / 2.0f - 1.0f);
        CHECK(six.oscillator().period() == static_cast<float>(fs) / 2.0f - 1.0f);
    }
}

TEST_CASE("phaser: sweep corners in the tan domain (FX-321) and the coefficient range", "[fx_mod][phaser]")
{
    using engine::fx::dsp::fdlibm::tanf;
    const float pi = std::bit_cast<float>(0x40490fdbu);
    for (bool parity : {false, true})
    {
        Phaser p;
        const float w0[6] = {0.3f, 0.0f, 0.0f, 0.6f, 0.0f, 1.0f}; // Width 0: no sweep
        setup(p, 44100.0, kQuirkProfileSixteen, w0, 0, parity);
        CHECK(p.lowCorner() == p.highCorner());
        const float f = Phaser::frequencyHz(0.6f, parity);
        CHECK(p.highCorner() == Catch::Approx(std::tan(pi * f / 44100.0f)).epsilon(1e-6));
        const float w1[6] = {0.3f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f}; // Width 1, 18 kHz: down to 100 Hz
        setup(p, 44100.0, kQuirkProfileSixteen, w1, 0, parity);
        CHECK(p.lowCorner() == Catch::Approx(std::tan(pi * 100.0f / 44100.0f)).epsilon(1e-6));
        CHECK(p.coefficientLow() < p.coefficientHigh());
        CHECK(p.coefficientHigh() < 0.95f);
    }
    // The frequency law: 300·60^(i/256), 257 steps from 300 Hz to 18 kHz
    CHECK(Phaser::frequencyHz(0.0f, true) == 300.0f);
    CHECK(Phaser::frequencyHz(1.0f, true) == Catch::Approx(18000.0f).epsilon(1e-6));
    CHECK(Phaser::frequencyHz(0.5f, false) == Catch::Approx(300.0 * std::sqrt(60.0)).epsilon(1e-6));
    // the fdlibm tanf port against double tan over the Phaser's range: within 1 ulp
    for (float x = 0.001f; x < 1.43f; x += 0.0173f)
        CHECK(std::fabs(tanf(x) - static_cast<float>(std::tan(static_cast<double>(x)))) <=
              std::nextafter(std::fabs(tanf(x)), 1e30f) - std::fabs(tanf(x)));
}

TEST_CASE("phaser: the 192-sample ramps of the coefficient and the feedback (FX-322, FX-324)", "[fx_mod][phaser]")
{
    const float k[6] = {0.2f, 0.8f, 0.0f, 0.7f, 0.4f, 1.0f};
    Phaser p;
    setup(p, 44100.0, kQuirkProfileSixteen, k);
    CHECK(p.feedbackNow() == 0.4f * 0.99f); // reset: fb := its target, ramps idle (FX-361)
    run(p, 4); // the first block's changes (against the constructor values) ramp over 192 samples
    CHECK(p.coefficient(0) == p.coefficientTarget(0));
    run(p, 1); // no change: a ramps over the block without the snap
    CHECK(p.coefficient(0) == Catch::Approx(p.coefficientTarget(0)).margin(1e-6));
    p.setParam(4, 1.0f);
    run(p, 1);
    const float fb1 = p.feedbackNow();
    CHECK(fb1 > 0.4f * 0.99f);
    CHECK(fb1 < 0.99f);
    CHECK(fb1 == Catch::Approx(0.396 + (0.99 - 0.396) / 3.0).margin(1e-4));
    run(p, 2);
    CHECK(p.feedbackNow() == 0.99f); // snapped to the target after 3 blocks
    p.setParam(3, 0.2f); // a Frequency change: 192-sample coefficient ramp, then the snap
    run(p, 2);
    CHECK(p.coefficient(0) != p.coefficientTarget(0));
    run(p, 1);
    CHECK(p.coefficient(0) == p.coefficientTarget(0));
}

TEST_CASE("phaser: retrigger restarts the LFO at its peak (FX-360); reset re-seeds (FX-361)", "[fx_mod][phaser]")
{
    Phaser p;
    setup(p, 44100.0, kQuirkProfileSixteen, kDefaults);
    run(p, 37);
    p.retrigger();
    run(p, 1);
    CHECK(p.lfoL()[0] > 0.999f); // the triangle 0…1 starts at 1
    CHECK(p.lfoL()[0] <= 1.0f);
    CHECK(p.lfoL()[N - 1] < p.lfoL()[0]);
    p.setOption(kRetrigger, 1);
    run(p, 1);
    CHECK(p.lfoL()[0] > 0.999f);
    // Phase 1 (180°): R leads by half a cycle, so it starts at the trough after the retrigger
    const float k[6] = {199.0f / 999.0f, 1.0f, 1.0f, 1.0f, 0.0f, 1.0f};
    setup(p, 44100.0, kQuirkProfileSixteen, k);
    run(p, 3);
    p.retrigger();
    run(p, 1);
    CHECK(p.lfoR()[0] < 0.001f);
}

TEST_CASE("phaser: with the coefficient frozen the notches follow six-stage all-pass theory (FX-325)",
          "[fx_mod][phaser]")
{
    // Width 0 (no sweep), feedback 0, Mix 1: wet = (in + AP⁶(in))·g has its first notch where the chain's phase is −π.
    const float k[6] = {0.3f, 0.0f, 0.0f, 128.0f / 256.0f, 0.0f, 1.0f};
    Phaser p;
    setup(p, 44100.0, kQuirkProfileSixteen, k);
    run(p, 8);
    const double a = p.coefficient(0);
    auto chainPhase = [a](double w)
    {
        const std::complex<double> z1 = std::polar(1.0, -w);
        return 6.0 * std::arg((a + z1) / (1.0 + a * z1));
    };
    double lo = 1e-4, hi = 3.1;
    for (int i = 0; i < 100; ++i) // the first crossing of −π (the phase falls monotonically from 0 to −6π)
    {
        const double mid = 0.5 * (lo + hi);
        double ph = 0;
        for (int s = 1; s <= 64; ++s) // unwrap along [0, mid]
        {
            const double w0 = mid * (s - 1) / 64.0, w1 = mid * s / 64.0;
            double d = chainPhase(w1) - chainPhase(w0);
            while (d > M_PI)
                d -= 2 * M_PI;
            while (d < -M_PI)
                d += 2 * M_PI;
            ph += d;
        }
        (ph > -M_PI ? lo : hi) = mid;
    }
    const double notchHz = lo * 44100.0 / (2 * M_PI);
    auto rmsAt = [&](double hz)
    {
        Phaser q;
        setup(q, 44100.0, kQuirkProfileSixteen, k);
        run(q, 8);
        std::vector<float> x(static_cast<std::size_t>(200 * N));
        for (std::size_t i = 0; i < x.size(); ++i)
            x[i] = 0.5f * static_cast<float>(std::sin(2 * M_PI * hz * static_cast<double>(i) / 44100.0));
        const auto y = runDirect(q, x, x, 44100.0);
        double s = 0;
        int n = 0;
        for (int b = 100; b < 200; ++b)
            for (int i = 0; i < N; ++i, ++n)
                s += static_cast<double>(y[static_cast<std::size_t>(2 * b * N + i)]) *
                     static_cast<double>(y[static_cast<std::size_t>(2 * b * N + i)]);
        return std::sqrt(s / n);
    };
    INFO("first notch " << notchHz << " Hz, a " << a);
    CHECK(rmsAt(notchHz) < 0.01 * rmsAt(notchHz * 0.5));
    CHECK(rmsAt(notchHz) < 0.01 * rmsAt(notchHz * 1.6));
}

TEST_CASE("phaser: parity maths switches tan/pow to the fdlibm ports; kClearState = a fresh instance",
          "[fx_mod][phaser]")
{
    const float k[6] = {0.3f, 0.7f, 0.0f, 212.0f / 256.0f, 0.3f, 1.0f};
    Phaser a, b;
    setup(a, 44100.0, kQuirkProfileReference, k, 0, true);
    setup(b, 44100.0, kQuirkProfileReference, k, 0, false);
    const auto inL = noise(40 * N, 9), inR = noise(40 * N, 10);
    const auto ya = runDirect(a, inL, inR, 44100.0), yb = runDirect(b, inL, inR, 44100.0);
    double maxDiff = 0;
    for (std::size_t i = 0; i < ya.size(); ++i)
        maxDiff = std::max(maxDiff, std::fabs(static_cast<double>(ya[i]) - static_cast<double>(yb[i])));
    CHECK(maxDiff < 1e-4); // the default-maths tolerance
    // kClearState rebuilds as constructed: the same output as a fresh instance afterwards
    Phaser fresh;
    setup(fresh, 44100.0, kQuirkProfileReference, k, 0, true);
    a.setOption(kClearState, 1);
    for (int i = 0; i < 6; ++i)
        a.setParam(i, k[i]);
    a.reset();
    CHECK(sameBits(runDirect(a, inL, inR, 44100.0), runDirect(fresh, inL, inR, 44100.0)));
}

TEST_CASE("phaser: registration — descriptor, ParamSpecs, defaults (FX-362)", "[fx_mod][phaser]")
{
    const auto* d = EffectRegistry::builtin().find("phaser");
    REQUIRE(d != nullptr);
    CHECK(d->name == "PHASER");
    CHECK(d->slots == kAllSlotKinds);
    CHECK(d->params[3] == "fx.phaser.frequency");
    CHECK(d->params[4] == "fx.phaser.feedback");
    REQUIRE(d->options.size() == 1);
    CHECK(d->options[0] == "fx.phaser.sync");
    CHECK(d->shell.mixLaw == static_cast<std::uint8_t>(MixLaw::CrossfadeK5));
    CHECK(d->shell.wetHighPass);
    const auto& reg = core::builtinParams();
    for (const char* slot : {"fx1", "fx2", "send", "master"})
    {
        const std::string p(slot);
        REQUIRE(reg.find(p + ".phaser.rate") != nullptr);
        CHECK(reg.find(p + ".phaser.rate")->defaultValue == 199.0f / 999.0f);
        CHECK(reg.find(p + ".phaser.rate.div") != nullptr);
        CHECK(reg.find(p + ".phaser.frequency")->defaultValue == 1.0f);
        CHECK(reg.find(p + ".phaser.width")->defaultValue == 1.0f);
        CHECK(reg.find(p + ".phaser.mix")->defaultValue == 0.0f);
    }
    char buf[32];
    reg.find("fx1.phaser.frequency")->format(1.0f, buf, static_cast<int>(sizeof buf));
    CHECK(std::string(buf) == "18.0 kHz");
    reg.find("fx1.phaser.frequency")->format(0.0f, buf, static_cast<int>(sizeof buf));
    CHECK(std::string(buf) == "300 Hz");
}
