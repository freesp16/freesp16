// Distortion (spec distortion.md): the quirk bits (the reference's fixed 44.1 kHz kernel, FX-1082
// constructor coefficients) show exactly their documented differences, the auto-gain (FX-1021), the clipper
// (FX-1022/1023) and the registration. The bit-exact comparison with distortion.py is tests/golden/distortion; the
// NEON ↔ scalar parity is test_distortion_neon.cpp.
#include "tests/unit/fx_color/ColorTestUtil.h"

#include "core/params/ParamRegistry.h"
#include "core/params/catalogue/fx.h"
#include "engine/fx/EffectRegistry.h"
#include "engine/fx/effects/Distortion.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace engine::fx;
using namespace colortest;

namespace
{
void defaults(EffectBase& e) // k order: drive, tone, lo, hi, dynamics, mix
{
    const float d[6] = {0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f};
    for (int k = 0; k < 6; ++k)
        e.setParam(k, d[k]);
}
std::vector<float> sine(int n, double f, double fs, float a)
{
    std::vector<float> v(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        v[static_cast<std::size_t>(i)] = a * static_cast<float>(std::sin(2.0 * 3.141592653589793 * f * i / fs));
    return v;
}
} // namespace

TEST_CASE("quirk_distortion: the reference kernel runs at 44,100 Hz whatever fs (Sixteen: the real fs)",
          "[fx_color][distortion]")
{
    const auto in = noise(16 * N, 11, 0.3f);
    for (double fs : {44100.0, 48000.0, 96000.0})
    {
        Distortion ref, six;
        for (auto* e : {&ref, &six})
        {
            e->prepare(fs, N);
            defaults(*e);
            e->setParam(0, 0.5f);
            e->setParam(4, 0.3f);
        }
        ref.setQuirkProfile(kDistQuirkFixed44k);
        six.setQuirkProfile(0);
        ref.reset();
        six.reset();
        INFO("fs " << fs);
        CHECK(ref.kernelRate() == 44100);
        CHECK(six.kernelRate() == static_cast<int>(fs));
        const auto a = runDirect(ref, in, in, fs), b = runDirect(six, in, in, fs);
        CHECK(sameBits(a, b) == (fs == 44100.0)); // identical exactly at 44.1 kHz
    }
}

TEST_CASE("quirk_distortion: FX-1082 constructor coefficients are 1 ulp off the setters' and only until a change",
          "[fx_color][distortion]")
{
    const auto in = noise(16 * N, 12, 0.5f);
    Distortion ref, six;
    for (auto* e : {&ref, &six})
    {
        e->prepare(44100.0, N);
        defaults(*e); // tone and hi freq at 20 kHz = the constructor values: their setters skip, ctor coefs stay
    }
    ref.setQuirkProfile(kDistQuirkCtorCoefs);
    six.setQuirkProfile(0);
    ref.reset();
    six.reset();
    const auto a = runDirect(ref, in, in, 44100.0), b = runDirect(six, in, in, 44100.0);
    double worst = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
        worst = std::max(worst, std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i])));
    CHECK(worst <= 1e-5); // a golden-only detail
    // Any value other than the constructor's runs the setter: the profiles then agree bit for bit.
    for (auto* e : {&ref, &six})
    {
        e->setParam(1, 0.9f);
        e->setParam(3, 0.95f);
    }
    const auto c = runDirect(ref, in, in, 44100.0), d = runDirect(six, in, in, 44100.0);
    // the filter states differ from the first run, so compare after both have settled on the same input
    const auto c2 = runDirect(ref, in, in, 44100.0), d2 = runDirect(six, in, in, 44100.0);
    double worst2 = 0.0;
    for (std::size_t i = a.size() / 2; i < c2.size(); ++i)
        worst2 = std::max(worst2, std::fabs(static_cast<double>(c2[i]) - static_cast<double>(d2[i])));
    CHECK(worst2 <= 1e-6);
    (void) c;
    (void) d;
}

TEST_CASE("distortion: stereo-linked auto-gain rises to 100 on quiet input and stays at 1 when loud (FX-1021)",
          "[fx_color][distortion]")
{
    const double fs = 44100.0;
    Distortion q;
    q.prepare(fs, N);
    defaults(q);
    q.setParam(0, 0.25f);
    q.setParam(4, 0.1f); // T = 0.1
    q.reset();
    const auto quiet = sine(400 * N, 220.0, fs, 0.0005f);
    runDirect(q, quiet, quiet, fs);
    CHECK(q.autoGain() == 100.0f); // capped (inc: one e-fold per 100 ms → 100 within ≈ 0.5 s)
    Distortion l;
    l.prepare(fs, N);
    defaults(l);
    l.setParam(4, 0.5f);
    l.reset();
    const auto loud = noise(64 * N, 13, 0.9f);
    runDirect(l, loud, loud, fs);
    CHECK(l.autoGain() == 1.0f);
    Distortion off; // Dynamics 0 = off: the gain can never rise
    off.prepare(fs, N);
    defaults(off);
    off.reset();
    runDirect(off, quiet, quiet, fs);
    CHECK(off.autoGain() == 1.0f);
    // One gain for both channels: a loud R keeps the quiet L from being lifted.
    Distortion st;
    st.prepare(fs, N);
    defaults(st);
    st.setParam(4, 0.1f);
    st.reset();
    runDirect(st, quiet, noise(400 * N, 13, 0.9f), fs); // as long as `quiet` (runDirect reads both channels in full)
    CHECK(st.autoGain() == 1.0f);
}

TEST_CASE("distortion: the soft clipper — ceiling 0.7, small-signal gain 1.05, the cube blend (FX-1022/1023, FX-1080)",
          "[fx_color][distortion]")
{
    CHECK(Distortion::shape(2.0f, 1.0f, 0.0f) == 0.7f);
    CHECK(Distortion::shape(-3.0f, 1.0f, 1.0f) == -0.7f);
    CHECK(Distortion::shape(0.0f, 100.0f, 1.0f) == 0.0f);
    const float g = Distortion::shape(1e-3f, 1.0f, 0.0f) / 1e-3f;
    CHECK(std::fabs(g - 1.05f) < 1e-4f); // 1.5 · 0.7
    // From 20 dB the blend is 1: quiet signals are pulled toward the raw cube (the dead zone, kept as character).
    const float soft = Distortion::shape(1e-4f, 10.0f, 0.0f), dead = Distortion::shape(1e-4f, 10.0f, 1.0f);
    CHECK(std::fabs(dead) < 0.1f * std::fabs(soft));
    // odd symmetry
    for (float x : {0.01f, 0.2f, 0.5f, 0.9f})
        CHECK(Distortion::shape(-x, 3.0f, 0.5f) == -Distortion::shape(x, 3.0f, 0.5f));
}

TEST_CASE("distortion: registration — page order, internal indices, defaults", "[fx_color][distortion]")
{
    const auto* d = EffectRegistry::builtin().find("distortion");
    REQUIRE(d != nullptr);
    CHECK(d->params[0] == "fx.distortion.drive");
    CHECK(d->params[1] == "fx.distortion.loFreq");
    CHECK(d->params[4] == "fx.distortion.tone");
    CHECK(d->options.empty());
    CHECK(d->shell.wetHighPass);
    (void) core::builtinParams();
    const auto* set = core::fx::effectParams("distortion");
    REQUIRE(set != nullptr);
    const int ks[6] = {0, 2, 3, 4, 1, 5};
    for (std::size_t i = 0; i < 6; ++i)
        CHECK(set->params[i].k == ks[i]);
    const auto& reg = core::builtinParams();
    CHECK(reg.find("send.distortion.mix")->defaultValue == 1.0f);
    CHECK(reg.find("master.distortion.tone")->defaultValue == 1.0f);
    CHECK(reg.find("fx1.distortion.loFreq")->defaultValue == 0.0f);
    CHECK(reg.find("fx1.distortion.drive")->lockable);
    CHECK_FALSE(reg.find("send.distortion.drive")->lockable);
}
