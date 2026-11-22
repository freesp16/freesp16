// The pad/kit check in-process ("16 pads play a kit with clean levels"): the engine with the composition root's set-up
// (fullSetup: every binding and the FX slot bank) gets 16 distinct one-shots on tracks 1-16 through ObjectSwap, the
// pads are hit once each, in order, 250 ms apart from the TriggerSink (UI-118), and MAIN must show 16 hits — each the
// pad's own sample (its pitch) — no sample above -0.3 dBFS and no sample-to-sample step above -40 dBFS outside the
// sounding samples. The host smoke repeats this through the app, the `file` audio device and
// tools/analyze-wav.py --kit.
#include "tests/unit/engine/EngineTestUtil.h"
#include "tests/unit/voice/VoiceHarness.h"

#include "engine/voice/VoiceSwaps.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <vector>

namespace
{
constexpr int kHitFrames = 11025;  // 250 ms at 44.1 kHz (the check's spacing)
constexpr int kSampleFrames = 4410; // 100 ms one-shots (tools/gen-test-kit.py)

double hz(int pad) { return 220.0 * std::pow(2.0, pad / 12.0); } // 16 semitones from A3: one pitch per pad

std::vector<float> oneShot(int pad)
{
    // −6 dBFS sine with a 2 ms raised-cosine fade-in and a 15 ms fade-out (the samples themselves never click).
    auto s = voicetest::sine(kSampleFrames, hz(pad), voicetest::kRate, 0.5f);
    const int in = 88, out = 662;
    for (int i = 0; i < kSampleFrames; ++i)
    {
        double g = 1.0;
        if (i < in)
            g *= 0.5 - 0.5 * std::cos(M_PI * i / in);
        if (i >= kSampleFrames - out)
            g *= 0.5 - 0.5 * std::cos(M_PI * (kSampleFrames - 1 - i) / out);
        s[static_cast<std::size_t>(i)] *= static_cast<float>(g);
    }
    return s;
}

double dbfs(float v) { return 20.0 * std::log10(static_cast<double>(v)); }
} // namespace

TEST_CASE("trigger.engine: 16 pads play a 16-sample kit with clean levels (pad/kit check, in-process)",
          "[trigger][engine]")
{
    std::vector<std::unique_ptr<voicetest::TestBuffer>> kit;
    for (int p = 0; p < 16; ++p)
        kit.push_back(std::make_unique<voicetest::TestBuffer>(oneShot(p)));
    {
        engine::EngineOptions o;
        o.testTone = true; // as in the app: the tone goes once track 1 has its sample
        engine::Engine e(enginetest::fullSetup(), o);
        e.prepare(44100.0, 512);
        for (int t = 0; t < 16; ++t)
        {
            core::audio::retain(kit[static_cast<std::size_t>(t)]->get());
            REQUIRE(e.bridge().post(engine::ObjectSwap {engine::voice::trackSampleSlot(0, t),
                                                         kit[static_cast<std::size_t>(t)]->get()}));
        }
        enginetest::render(e, 4096, 256); // installs the kit; the test tone stops
        std::vector<float> l, r;
        for (int p = 0; p < 16; ++p)
        {
            REQUIRE(e.triggers().push({static_cast<std::uint8_t>(p), 110, 60, static_cast<std::int8_t>(p)}));
            const auto out = enginetest::render(e, kHitFrames, 256);
            l.insert(l.end(), out.l.begin(), out.l.end());
            r.insert(r.end(), out.r.begin(), out.r.end());
        }
        CHECK(e.voiceStarts() == 16);

        float worstStep = 0, worstPeak = 0;
        for (int p = 0; p < 16; ++p)
        {
            INFO("pad " << p + 1);
            const auto h0 = static_cast<std::size_t>(p * kHitFrames);
            const std::size_t sounding = h0 + kSampleFrames + 512; // the sample + one period of trigger latency
            CHECK(enginetest::peak(l, h0, sounding) > 0.05f); // it sounds…
            CHECK(std::fabs(voicetest::frequency(l, h0 + 1024, h0 + 3584) / hz(p) - 1.0) < 0.01); // …its own sample
            CHECK(enginetest::peak(l, sounding, h0 + kHitFrames) < 1e-4f); // and stops before the next hit
            for (const auto* ch : {&l, &r})
            {
                worstPeak = std::max(worstPeak, enginetest::peak(*ch, h0, h0 + kHitFrames));
                worstStep = std::max(worstStep, enginetest::maxStep(*ch, sounding, h0 + kHitFrames));
            }
        }
        INFO("peak " << dbfs(worstPeak) << " dBFS, largest step outside the samples " << dbfs(worstStep) << " dBFS");
        CHECK(dbfs(worstPeak) <= -0.3);
        CHECK(worstStep <= std::pow(10.0f, -40.0f / 20.0f));
        CHECK(enginetest::maxStep(l, 1) < 0.1f); // no discontinuity anywhere (a 262–523 Hz −6 dBFS sine steps ≤ 0.04)
    }
    for (const auto& b : kit)
        CHECK(b->refs() == 0); // the engine gave every buffer back
}
