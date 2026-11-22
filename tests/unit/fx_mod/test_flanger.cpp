// Flanger behaviour and quirks (spec flanger.md FX-220…224, FX-260…263, FX-280…282). The bit-exact
// comparison with the research model is tests/golden/flanger (Reference profile); these tests pin the documented
// differences of the Sixteen profile (one per quirk flag), the delay-dependent gain, the lifecycle and the registration.
#include "tests/unit/fx_mod/ModTestUtil.h"

#include "core/params/ParamRegistry.h"
#include "engine/fx/EffectRegistryOrder.h"
#include "engine/fx/FxShell.h"
#include "engine/fx/effects/Flanger.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <string>
#include <vector>

using namespace engine::fx;
using namespace modtest;

namespace
{
void setup(Flanger& f, double fs, std::uint32_t quirks, const float (&k)[6], int sync = 0)
{
    f.prepare(fs, N);
    for (int i = 0; i < 6; ++i)
        f.setParam(i, k[i]);
    f.setOption(Flanger::kOptSync, sync);
    f.setQuirkProfile(quirks);
    f.reset();
}

void collect(Flanger& f, int blocks, std::vector<float>& dl, std::vector<float>& dr, double fs = 44100.0)
{
    const auto ctx = context(fs);
    alignas(16) float l[N]{}, r[N]{};
    for (int b = 0; b < blocks; ++b)
    {
        f.process(l, r, ctx);
        dl.insert(dl.end(), f.delayL(), f.delayL() + N);
        dr.insert(dr.end(), f.delayR(), f.delayR() + N);
    }
}
} // namespace

TEST_CASE("quirk_flanger: FX-280 the Reference kernel runs at 44,100 Hz whatever fs (Sixteen: the real fs)",
          "[fx_mod][flanger]")
{
    const float k[6] = {1.0f, 1.0f, 0.0f, 1.0f, 0.5f, 1.0f};
    Flanger ref, six;
    setup(ref, 48000.0, kQuirkProfileReference, k);
    setup(six, 48000.0, kQuirkProfileSixteen, k);
    CHECK(ref.kernelRate() == 44100);
    CHECK(ref.ringLength() == 372); // 7·44100/1000 + 64
    CHECK(six.kernelRate() == 48000);
    CHECK(six.ringLength() == 400);
    std::vector<float> dl, dr;
    collect(ref, 2, dl, dr, 48000.0);
    collect(six, 2, dl, dr, 48000.0);
    CHECK(ref.oscillator().period() == 44100.0f / 1.0f - 1.0f);
    CHECK(six.oscillator().period() == 48000.0f / 1.0f - 1.0f);
    // the maximum sweep: 2·amp + 0.2 = 308.9 samples (7 ms at 44.1 kHz) vs 336.2 (Sixteen at 48 kHz)
    std::vector<float> rl, rr, sl, sr;
    Flanger r2, s2;
    setup(r2, 48000.0, kQuirkProfileReference, k);
    setup(s2, 48000.0, kQuirkProfileSixteen, k);
    collect(r2, 800, rl, rr, 48000.0);
    collect(s2, 800, sl, sr, 48000.0);
    CHECK(*std::max_element(rl.begin(), rl.end()) == Catch::Approx(308.9).margin(0.2));
    CHECK(*std::max_element(sl.begin(), sl.end()) == Catch::Approx(336.2).margin(0.2));
}

TEST_CASE("quirk_flanger: FX-281 phase divisor — R leads by deg/360 (Sixteen) or deg/380 (Reference) cycle",
          "[fx_mod][flanger]")
{
    // 1 Hz (v = 1): 44,100 samples per cycle. 180°: Sixteen 22,050 samples, Reference 20,889.5.
    const float k[6] = {1.0f, 1.0f, 1.0f, 0.5f, 0.0f, 1.0f};
    for (auto [quirks, expected] :
         {std::pair{kQuirkProfileSixteen, 22050.0}, std::pair{kQuirkProfileReference, 20889.5}})
    {
        Flanger f;
        setup(f, 44100.0, quirks, k);
        std::vector<float> dl, dr;
        collect(f, 800, dl, dr);
        const int lag = bestLag(dl, dr, 2000, 4410, 23000, 20000);
        INFO("profile " << quirks << " lag " << lag);
        CHECK(std::fabs(lag - expected) <= 1.0);
    }
}

TEST_CASE("quirk_flanger: F026 change trap — a first RATE of 0.5 keeps the constructor's 0.1 Hz (Reference only)",
          "[fx_mod][flanger]")
{
    const float k[6] = {0.5f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    Flanger ref, six;
    setup(ref, 44100.0, kQuirkProfileReference, k);
    setup(six, 44100.0, kQuirkProfileSixteen, k);
    std::vector<float> dl, dr;
    collect(ref, 1, dl, dr);
    collect(six, 1, dl, dr);
    CHECK(ref.rateHz() == std::bit_cast<float>(0x3dcccccdu)); // 0.1 Hz
    CHECK(six.rateHz() == 50.0f * 0.01f);                      // rint(50.5) = 50 → 0.50 Hz (FX-056)
}

TEST_CASE("flanger: the delay-dependent gain has a strict 48-sample knee (FX-223)", "[fx_mod][flanger]")
{
    CHECK(Flanger::delayGain(0.0f) == Catch::Approx(0.3686).margin(1e-6));
    CHECK(Flanger::delayGain(47.999996f) < 1.0f);
    CHECK(Flanger::delayGain(47.999996f) > 0.9999f);
    CHECK(Flanger::delayGain(48.0f) == Catch::Approx(1.0).margin(1e-6));
    CHECK(Flanger::delayGain(48.000126f) == 1.0f);
    CHECK(Flanger::delayGain(300.0f) == 1.0f);
}

TEST_CASE("flanger: no start-up mute, rates, feedback law, reset keeps y_prev and the feedback (FX-260…262)",
          "[fx_mod][flanger]")
{
    const float k[6] = {19.0f / 99.0f, 1.0f, 0.0f, 0.5f, 0.99f, 1.0f};
    for (std::uint32_t q : {kQuirkProfileReference, kQuirkProfileSixteen})
    {
        Flanger f;
        setup(f, 44100.0, q, k);
        const auto inL = noise(N, 3), inR = noise(N, 4);
        const auto y = runDirect(f, inL, inR, 44100.0);
        float peak = 0;
        for (int i = 0; i < N; ++i)
            peak = std::max(peak, std::fabs(y[static_cast<std::size_t>(i)]));
        CHECK(peak > 0.1f); // block 0 already has output (FX-260)
        CHECK(f.rateHz() == Catch::Approx(0.2f));
        CHECK(f.feedback() == 98.0f * std::bit_cast<float>(0x3c23d70au)); // rint(99·0.99) = 98 → 0.98
    }
    Flanger f;
    setup(f, 44100.0, kQuirkProfileSixteen, k);
    f.setParam(4, 1.0f);
    CHECK(f.feedback() == std::bit_cast<float>(0x3f7d70a3u)); // the 0.99 literal, at once
    const auto inL = noise(40 * N, 7), inR = noise(40 * N, 8);
    (void) runDirect(f, inL, inR, 44100.0);
    const float fb = f.feedback(), counter = f.oscillator().counter();
    f.reset();
    CHECK(f.feedback() == fb);
    CHECK(f.oscillator().counter() == counter);
    // retrigger: the LFO restarts at phase 0 at the next block (FX-262)
    f.retrigger();
    alignas(16) float l[N]{}, r[N]{};
    f.process(l, r, context(44100.0));
    CHECK(f.oscillator().counter() == static_cast<float>(N));
    // SYNC: 64 … 1 beats per cycle
    f.setOption(Flanger::kOptSync, 1);
    f.setParam(0, 1.0f);
    f.process(l, r, context(44100.0, 120.0));
    CHECK(f.rateHz() == Catch::Approx(2.0f).epsilon(1e-6));
    f.setParam(0, 0.0f);
    f.process(l, r, context(44100.0, 120.0));
    CHECK(f.rateHz() == Catch::Approx(120.0 / 60.0 / 64.0).epsilon(1e-6));
}

TEST_CASE("flanger: registration — descriptor, ParamSpecs, defaults (FX-200, FX-263)", "[fx_mod][flanger]")
{
    const auto* d = EffectRegistry::builtin().find("flanger");
    REQUIRE(d != nullptr);
    CHECK(d->name == "FLANGER");
    CHECK(d->slots == kAllSlotKinds);
    CHECK(d->params[4] == "fx.flanger.feedback");
    REQUIRE(d->options.size() == 1);
    CHECK(d->options[0] == "fx.flanger.sync");
    CHECK(d->visual == "lfoCurve");
    CHECK(d->shell.mixLaw == static_cast<std::uint8_t>(MixLaw::UnityDryK5));
    CHECK(d->shell.wetHighPass);
    const auto& reg = core::builtinParams();
    for (const char* slot : {"fx1", "fx2", "send", "master"})
    {
        const std::string p(slot);
        REQUIRE(reg.find(p + ".flanger.rate") != nullptr);
        CHECK(reg.find(p + ".flanger.rate")->defaultValue == 19.0f / 99.0f);
        CHECK(reg.find(p + ".flanger.rate.div") != nullptr);
        CHECK(reg.find(p + ".flanger.feedback")->defaultValue == 0.0f);
        CHECK(reg.find(p + ".flanger.mix")->defaultValue == 0.0f);
        CHECK(reg.find(p + ".flanger.sync")->type == core::ParamType::Bool);
    }
    char buf[32];
    reg.find("fx1.flanger.delay")->format(1.0f, buf, static_cast<int>(sizeof buf));
    CHECK(std::string(buf) == "7.0 ms");
    reg.find("fx1.flanger.feedback")->format(1.0f, buf, static_cast<int>(sizeof buf));
    CHECK(std::string(buf) == "99 %");
}
