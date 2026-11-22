// Audio quality of the voices: pitch accuracy (a 440 Hz sine at +1200 cents measures 880 Hz ± 0.5 Hz, PLY-020), the
// loop seam (PLY-011: a periodic loop stays below −60 dBFS of discontinuity energy; a misaligned loop's jump is spread
// over the 64-frame crossfade instead of a step), and the BROWSE preview voice (LIB-011).
#include "VoiceHarness.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace voicetest;
using engine::voice::TrackVoice;
using Catch::Matchers::WithinAbs;

namespace
{
// A naive sawtooth of `period` frames, −1 … +1.
std::vector<float> saw(int frames, double period, double phase = 0.0)
{
    std::vector<float> v(static_cast<std::size_t>(frames));
    for (int i = 0; i < frames; ++i)
    {
        const double t = std::fmod(i / period + phase, 1.0);
        v[static_cast<std::size_t>(i)] = static_cast<float>(2.0 * t - 1.0);
    }
    return v;
}

double db(double energyRatio) { return 10.0 * std::log10(energyRatio > 1e-30 ? energyRatio : 1e-30); }
} // namespace

TEST_CASE("voice.pitch: a 440 Hz sine at +1200 cents measures 880 Hz ± 0.5 Hz", "[voice][pitch]")
{
    TestBuffer b(sine(3 * 44100, 440.0));
    for (const auto quality : {engine::voice::InterpolatorKind::Hermite4, engine::voice::InterpolatorKind::Sinc16})
    {
        TrackVoice v;
        v.prepare(kRate, 64);
        v.setSample(b.get());
        v.setQuality(quality);
        const int blocks = 44100 / 64; // 1 s of output = 2 s of the sample
        const auto out = run(v, blocks, [&](int block) {
            if (block == 0)
                v.start(params(0, engine::voice::kWholeSample, false, 0, false, 1200.0f), 0.0f, 0, 1.0f);
        });
        const double hz = frequency(out.mainL, 0, out.size());
        INFO(engine::voice::interpolatorName(quality) << ": " << hz << " Hz");
        CHECK_THAT(static_cast<double>(hz), WithinAbs(880.0, 0.5));
    }
    // A semitone up and an octave down, too.
    for (const float cents : {100.0f, -1200.0f})
    {
        TrackVoice v;
        v.prepare(kRate, 64);
        v.setSample(b.get());
        const auto out = run(v, 44100 / 64, [&](int block) {
            if (block == 0)
                v.start(params(), cents, 0, 1.0f);
        });
        const double want = 440.0 * std::exp2(static_cast<double>(cents) / 1200.0);
        CHECK_THAT(static_cast<double>(frequency(out.mainL, 0, out.size())), WithinAbs(want, 0.5));
    }
}

TEST_CASE("voice.pitch: a buffer at another rate plays at its own pitch", "[voice][pitch]")
{
    TestBuffer b(sine(2 * 48000, 1000.0, 48000.0), {}, 48000.0);
    TrackVoice v;
    v.prepare(kRate, 64);
    v.setSample(b.get());
    const auto out = run(v, 44100 / 64, [&](int block) {
        if (block == 0)
            v.start(params(), 0.0f, 0, 1.0f);
    });
    CHECK_THAT(static_cast<double>(frequency(out.mainL, 0, out.size())), WithinAbs(1000.0, 0.5));
}

TEST_CASE("voice.loop: a periodic loop seam on a sawtooth stays below -60 dBFS of discontinuity energy",
          "[voice][loop]")
{
    // A 100-frame sawtooth; the loop [1037, 1837) holds exactly 8 periods and starts mid-ramp, so a correct seam
    // continues the waveform exactly and any misalignment of the crossfade shows up as error energy.
    const auto data = saw(4000, 100.0);
    TestBuffer b(data);
    for (const double rate : {1.0, 1.0594630943592953, 0.75})
        for (const bool reverse : {false, true})
        {
            TrackVoice v;
            v.prepare(kRate, 64);
            v.setSample(b.get());
            const float cents = static_cast<float>(1200.0 * std::log2(rate));
            const auto out = run(v, 120, [&](int block) {
                if (block == 0)
                    v.start(params(1037, 800, true, 1037, reverse), cents, 0, 1.0f);
            });
            // Ideal: the endless sawtooth at the played positions (the loop is periodic, so it is continuous).
            // Compare a window over several seams, away from the interpolator's own error at the saw's resets:
            // the error is measured as energy relative to full scale.
            double err = 0;
            int counted = 0;
            const double actual = std::exp2(static_cast<double>(cents) / 1200.0);
            for (std::size_t i = 0; i < out.size(); ++i)
            {
                const double travelled = actual * static_cast<double>(i);
                const double pos = reverse ? 1836.0 - travelled : 1037.0 + travelled;
                const double phase = std::fmod(std::fmod(pos / 100.0, 1.0) + 1.0, 1.0);
                if (phase < 0.04 || phase > 0.96)
                    continue; // near the saw's own reset the interpolated value is not the ideal ramp
                const double ideal = 2.0 * phase - 1.0;
                const double e = static_cast<double>(out.mainL[i]) - ideal;
                err += e * e;
                ++counted;
            }
            const double energy = db(err / counted);
            INFO("rate " << rate << " reverse " << reverse << ": " << energy << " dBFS");
            CHECK(energy < -60.0);
        }
}

TEST_CASE("voice.loop: a misaligned loop's jump is spread over the crossfade", "[voice][loop]")
{
    // The loop [1045, 1770) holds 7.25 periods of a 100-frame sawtooth (resets at 75 mod 100, none inside either
    // seam zone): a hard jump would step by 0.5.
    const auto data = saw(4000, 100.0, 0.25);
    TestBuffer b(data);
    TrackVoice v;
    v.prepare(kRate, 64);
    v.setSample(b.get());
    const auto out = run(v, 60, [&](int block) {
        if (block == 0)
            v.start(params(1045, 725, true, 1045), 0.0f, 0, 1.0f);
    });
    // Away from the sawtooth's own resets (every 100 frames), steps stay near its slope 0.02 plus the crossfade's
    // spread of the 0.5 offset over 64 frames.
    float worst = 0;
    for (std::size_t i = 1; i < out.size(); ++i)
    {
        const float step = std::fabs(out.mainL[i] - out.mainL[i - 1]);
        if (step < 1.0f) // the saw's own resets are steps of ≈ 2
            worst = std::max(worst, step);
    }
    INFO("largest non-reset step " << worst);
    CHECK(worst <= 0.02f + 0.5f / 64.0f + 1e-4f);
}

TEST_CASE("voice.preview: plays once into aux, mono to both sides, with its gain", "[voice][preview]")
{
    TestBuffer b(std::vector<float>(1000, 0.8f));
    engine::voice::PreviewVoice pv;
    pv.prepare(kRate);
    pv.setGain(0.5f);
    std::vector<float> l(64 * 20, 0.25f), r(64 * 20, 0.25f);
    for (int block = 0; block < 20; ++block)
    {
        core::rt::ScopedRealtimeThread realtime;
        pv.install(b.get()); // the slot content every block: the same pointer does not restart
        pv.render(&l[static_cast<std::size_t>(block * 64)], &r[static_cast<std::size_t>(block * 64)]);
    }
    // Block 0 ramps the gain from 1 to 0.5, then 0.8 × 0.5 is added to the 0.25 already there.
    CHECK_THAT(static_cast<double>(l[100]), WithinAbs(0.25 + 0.4, 1e-6));
    CHECK_THAT(static_cast<double>(r[999]), WithinAbs(0.25 + 0.4, 1e-6));
    CHECK(l[1000] == 0.25f); // played once
    CHECK_FALSE(pv.active());
    CHECK(b.refs() == 0);
}

TEST_CASE("voice.preview: a new preview fades the old one out over 2 ms", "[voice][preview]")
{
    TestBuffer a(std::vector<float>(10000, 1.0f));
    TestBuffer c(std::vector<float>(10000, -1.0f));
    engine::voice::PreviewVoice pv;
    pv.prepare(kRate);
    std::vector<float> l(64 * 6, 0.0f), r(64 * 6, 0.0f);
    for (int block = 0; block < 6; ++block)
    {
        core::rt::ScopedRealtimeThread realtime;
        pv.install(block < 2 ? a.get() : (block < 4 ? c.get() : nullptr));
        pv.render(&l[static_cast<std::size_t>(block * 64)], &r[static_cast<std::size_t>(block * 64)]);
    }
    CHECK(l[127] == 1.0f);
    CHECK_THAT(static_cast<double>(l[128]), WithinAbs(87.0 / 88.0 - 1.0, 1e-6)); // old fading + new at full level
    CHECK(l[128 + 88] == -1.0f);
    CHECK(l[256 + 88] == 0.0f); // install(nullptr): faded out
    CHECK(a.refs() == 0);
    CHECK(c.refs() == 0);
}
