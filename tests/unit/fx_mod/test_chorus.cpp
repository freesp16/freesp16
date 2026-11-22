// Chorus behaviour and quirks (spec chorus.md FX-120…124, FX-160…163, FX-180…182). The bit-exact
// comparison with the research model is tests/golden/chorus (Reference profile); these tests pin the documented
// differences of the Sixteen profile (one per quirk flag), the timing rules and the registration.
#include "tests/unit/fx_mod/ModTestUtil.h"

#include "core/params/ParamRegistry.h"
#include "core/params/catalogue/fx.h"
#include "engine/fx/EffectRegistryOrder.h"
#include "engine/fx/FxShell.h"
#include "engine/fx/effects/Chorus.h"

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
void setup(Chorus& c, double fs, std::uint32_t quirks, const float (&k)[6], int sync = 0)
{
    c.prepare(fs, N);
    for (int i = 0; i < 6; ++i)
        c.setParam(i, k[i]);
    c.setOption(Chorus::kOptSync, sync);
    c.setQuirkProfile(quirks);
    c.reset();
}

// Collects the LFO delay curves (L, R) of `blocks` blocks.
void collect(Chorus& c, int blocks, std::vector<float>& dl, std::vector<float>& dr, double fs = 44100.0)
{
    const auto ctx = context(fs);
    alignas(16) float l[N]{}, r[N]{};
    for (int b = 0; b < blocks; ++b)
    {
        c.process(l, r, ctx);
        dl.insert(dl.end(), c.delayL(), c.delayL() + N);
        dr.insert(dr.end(), c.delayR(), c.delayR() + N);
    }
}

constexpr float kDefaults[6] = {19.0f / 99.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
} // namespace

TEST_CASE("quirk_chorus: FX-180 start-up mute — the Sixteen output differs from the Reference only in blocks 0–76",
          "[fx_mod][chorus]")
{
    Chorus ref, six;
    setup(ref, 44100.0, kQuirkProfileReference, kDefaults);
    setup(six, 44100.0, kQuirkProfileSixteen, kDefaults);
    const auto inL = noise(120 * N, 1), inR = noise(120 * N, 2);
    const auto a = runDirect(ref, inL, inR, 44100.0), b = runDirect(six, inL, inR, 44100.0);
    int lastDiff = -1;
    for (int blk = 0; blk < 120; ++blk)
        if (! sameBits(a.data() + 2 * blk * N, b.data() + 2 * blk * N, 2 * N))
            lastDiff = blk;
    CHECK(lastDiff == 76);
    // Reference: silent until the ring (4,474 samples) has wrapped once (block 69), then the ramp; Sixteen: g = 1
    for (int i = 0; i < 69 * N; ++i)
        REQUIRE(a[static_cast<std::size_t>(2 * (i / N) * N + i % N)] == 0.0f);
    CHECK(a[static_cast<std::size_t>(2 * 70 * N)] != 0.0f);
    CHECK(ref.outputGain() == 1.0f);
    float peak = 0;
    for (int i = 0; i < N; ++i)
        peak = std::max(peak, std::fabs(b[static_cast<std::size_t>(i)]));
    CHECK(peak > 0.0f); // Sixteen: the very first block already has wet output
}

TEST_CASE("quirk_chorus: FX-181 phase divisor — R leads by deg/360 (Sixteen) or deg/380 (Reference) cycle",
          "[fx_mod][chorus]")
{
    // 10 Hz (v = 1): the sine recursion's cycle is 4,410 samples. Phase 180°: Sixteen 0.5 cycle = 2,205 samples,
    // Reference 180/380 cycle = 2,088.9 samples.
    const float k[6] = {1.0f, 1.0f, 1.0f, 0.5f, 0.0f, 1.0f};
    for (auto [quirks, expected] : {std::pair{kQuirkProfileSixteen, 2205.0}, std::pair{kQuirkProfileReference, 2088.9}})
    {
        Chorus c;
        setup(c, 44100.0, quirks, k);
        std::vector<float> dl, dr;
        collect(c, 260, dl, dr);
        const int lag = bestLag(dl, dr, 2000, 4410, 4409);
        INFO("profile " << quirks << " lag " << lag);
        CHECK(std::fabs(lag - expected) <= 1.0);
    }
    // Phase 90°: a quarter cycle (Sixteen)
    const float k90[6] = {1.0f, 1.0f, 0.5f, 0.5f, 0.0f, 1.0f};
    Chorus c;
    setup(c, 44100.0, kQuirkProfileSixteen, k90);
    std::vector<float> dl, dr;
    collect(c, 260, dl, dr);
    CHECK(std::abs(bestLag(dl, dr, 2000, 4410, 4409) - 1102) <= 1);
}

TEST_CASE("quirk_chorus: FX-182 the Reference kernel runs at 44,100 Hz whatever fs (Sixteen: the real fs)",
          "[fx_mod][chorus]")
{
    const float k[6] = {1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f}; // 10 Hz, width 100, delay x10
    Chorus ref, six;
    setup(ref, 48000.0, kQuirkProfileReference, k);
    setup(six, 48000.0, kQuirkProfileSixteen, k);
    CHECK(ref.kernelRate() == 44100);
    CHECK(ref.ringLength() == 4474);
    CHECK(six.kernelRate() == 48000);
    CHECK(six.ringLength() == 4864);
    std::vector<float> dl, dr;
    collect(ref, 2, dl, dr, 48000.0);
    collect(six, 2, dl, dr, 48000.0);
    CHECK(ref.oscillator().period() == 44100.0f / 10.0f - 1.0f); // FX-1380 inside FX-182
    CHECK(six.oscillator().period() == 48000.0f / 10.0f - 1.0f);
    // the sweep in samples scales with the kernel rate: max 2·amp + 0.2 = 441.2 (Reference) vs 480.2 (Sixteen), at
    // 1 Hz (the 50 Hz smoother passes it at 0.9998)
    const float k1[6] = {9.0f / 99.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f};
    Chorus r2, s2;
    setup(r2, 48000.0, kQuirkProfileReference, k1);
    setup(s2, 48000.0, kQuirkProfileSixteen, k1);
    std::vector<float> rl, rr, sl, sr;
    collect(r2, 800, rl, rr, 48000.0);
    collect(s2, 800, sl, sr, 48000.0);
    CHECK(*std::max_element(rl.begin(), rl.end()) == Catch::Approx(441.2).margin(0.1));
    CHECK(*std::max_element(sl.begin(), sl.end()) == Catch::Approx(480.2).margin(0.1));
    // a profile change of a running instance is honoured at the next reset (FX-053): the kernel is rebuilt
    s2.setQuirkProfile(kQuirkProfileReference);
    s2.reset();
    CHECK(s2.kernelRate() == 44100);
}

TEST_CASE("quirk_chorus: F026 change trap — a first RATE of 0.5 keeps the constructor's 0.1 Hz (Reference only)",
          "[fx_mod][chorus]")
{
    const float k[6] = {0.5f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    Chorus ref, six;
    setup(ref, 44100.0, kQuirkProfileReference, k);
    setup(six, 44100.0, kQuirkProfileSixteen, k);
    std::vector<float> dl, dr;
    collect(ref, 1, dl, dr);
    collect(six, 1, dl, dr);
    CHECK(ref.rateHz() == std::bit_cast<float>(0x3dcccccdu)); // 0.1 Hz
    CHECK(six.rateHz() == 5.0f);                               // rint(1 + 49.5) = 50 → 5.0 Hz (FX-056)
}

TEST_CASE("chorus: rate laws — free 0.1 Hz steps, SYNC divisions, the 64 Hz clamp, applied at the next block",
          "[fx_mod][chorus]")
{
    Chorus c;
    const float k[6] = {19.0f / 99.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    setup(c, 44100.0, kQuirkProfileSixteen, k);
    const auto ctx120 = context(44100.0, 120.0), ctx999 = context(44100.0, 999.0);
    alignas(16) float l[N]{}, r[N]{};
    c.process(l, r, ctx120);
    CHECK(c.rateHz() == Catch::Approx(2.0f)); // default 19/99 → 2.0 Hz
    c.setParam(0, 1.0f);
    CHECK(c.rateHz() == Catch::Approx(2.0f)); // FX-160: not before the next block
    c.process(l, r, ctx120);
    CHECK(c.rateHz() == Catch::Approx(10.0f));
    c.setOption(Chorus::kOptSync, 1);
    c.setParam(0, 4.0f / 9.0f); // 1 beat per cycle
    c.process(l, r, ctx120);
    CHECK(c.rateHz() == Catch::Approx(2.0f).epsilon(1e-6));
    c.setParam(0, 1.0f); // 1/16 beat at 999 BPM = 266 Hz → 64 Hz (FX-121)
    c.process(l, r, ctx999);
    CHECK(c.rateHz() == 64.0f);
    c.setParam(0, 0.0f); // 16 beats at 999 BPM
    c.process(l, r, ctx999);
    CHECK(c.rateHz() == Catch::Approx(999.0 / 60.0 / 16.0).epsilon(1e-6));
    c.process(l, r, ctx120); // the tempo is read every block (FX-049)
    CHECK(c.rateHz() == Catch::Approx(120.0 / 60.0 / 16.0).epsilon(1e-6));
}

TEST_CASE("chorus: depth law — the sweep runs from 0.2 to 2·amp + 0.2 samples (FX-120)", "[fx_mod][chorus]")
{
    for (auto [scaleV, maxD] : {std::pair{0.0f, 44.3}, std::pair{1.0f, 441.2}})
    {
        const float k[6] = {9.0f / 99.0f, 1.0f, 0.0f, scaleV, 0.0f, 1.0f}; // 1 Hz
        Chorus c;
        setup(c, 44100.0, kQuirkProfileSixteen, k);
        std::vector<float> dl, dr;
        collect(c, 800, dl, dr);
        const auto mm = std::minmax_element(dl.begin() + 2000, dl.end());
        CHECK(*mm.first == Catch::Approx(0.2).margin(0.1));
        CHECK(*mm.second == Catch::Approx(maxD).margin(0.1));
    }
}

TEST_CASE("chorus: retrigger restarts the LFO at phase 0 at the next block; reset keeps the index, phase and g "
          "(FX-161/162)",
          "[fx_mod][chorus]")
{
    Chorus c;
    setup(c, 44100.0, kQuirkProfileReference, kDefaults);
    const auto inL = noise(100 * N, 5), inR = noise(100 * N, 6);
    (void) runDirect(c, inL, inR, 44100.0);
    const float counter = c.oscillator().counter(), g = c.outputGain();
    c.reset(); // activation: rings and LFO buffers zeroed only
    CHECK(c.oscillator().counter() == counter);
    CHECK(c.outputGain() == g);
    alignas(16) float l[N]{}, r[N]{};
    const auto ctx = context(44100.0);
    c.process(l, r, ctx);
    for (int i = 0; i < N; ++i) // zeroed rings, silent input → silence
        REQUIRE((l[i] == 0.0f && r[i] == 0.0f));
    c.retrigger();
    c.process(l, r, ctx);
    CHECK(c.oscillator().counter() == static_cast<float>(N)); // restarted at 0, then one block
    c.setOption(kRetrigger, 1);                               // the reserved option code does the same
    c.process(l, r, ctx);
    CHECK(c.oscillator().counter() == static_cast<float>(N));
}

TEST_CASE("chorus: registration — descriptor, ParamSpecs, defaults (FX-100, FX-163)", "[fx_mod][chorus]")
{
    const auto* d = EffectRegistry::builtin().find("chorus");
    REQUIRE(d != nullptr);
    CHECK(d->name == "CHORUS");
    CHECK(d->slots == kAllSlotKinds);
    CHECK(d->params[0] == "fx.chorus.rate");
    CHECK(d->params[3] == "fx.chorus.delay");
    CHECK(d->params[4].empty());
    CHECK(d->params[5] == "fx.chorus.mix");
    REQUIRE(d->options.size() == 1);
    CHECK(d->options[0] == "fx.chorus.sync");
    CHECK(d->visual == "lfoCurve");
    CHECK(d->shell.mixLaw == static_cast<std::uint8_t>(MixLaw::UnityDryK5));
    CHECK(d->shell.wetHighPass);
    CHECK(d->shell.gateMode == static_cast<std::uint8_t>(GateMode::Open));
    const auto& reg = core::builtinParams();
    for (const char* slot : {"fx1", "fx2", "send", "master"})
    {
        const std::string p(slot);
        const auto* rate = reg.find(p + ".chorus.rate");
        REQUIRE(rate != nullptr);
        CHECK(rate->defaultValue == 19.0f / 99.0f);
        CHECK(reg.find(p + ".chorus.rate.div") != nullptr);
        CHECK(reg.find(p + ".chorus.width")->defaultValue == 1.0f);
        CHECK(reg.find(p + ".chorus.mix")->defaultValue == 0.0f); // Mix 0 everywhere (FX-163)
        const auto* sync = reg.find(p + ".chorus.sync");
        REQUIRE(sync != nullptr);
        CHECK(sync->type == core::ParamType::Bool);
        CHECK(rate->lockable == (p == "fx1" || p == "fx2"));
    }
    char buf[32];
    reg.find("fx1.chorus.delay")->format(1.0f, buf, static_cast<int>(sizeof buf));
    CHECK(std::string(buf) == "10.0 ms");
    reg.find("fx1.chorus.phase")->format(1.0f, buf, static_cast<int>(sizeof buf));
    CHECK(std::string(buf) == "180\xc2\xb0");
}
