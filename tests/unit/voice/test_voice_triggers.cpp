// Triggers and voice hand-off (PLY-040…044, PLY-022): sample-accurate start inside the block, the 2 ms release tail
// on retrigger (no step above 1/88 of full scale on a full-scale DC sample), stop / choke fades, the 64-frame cut of a
// tail on a fast third trigger, bypass, sample swaps with reference counting, pitch modulation, and no allocation
// after prepare (the whole run is on a real-time-marked thread: APP_RT_CHECKS aborts on allocation).
#include "VoiceHarness.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace voicetest;
using engine::voice::TrackVoice;
using Catch::Matchers::WithinAbs;

namespace
{
float maxStep(const std::vector<float>& x, std::size_t from, std::size_t to)
{
    float worst = 0;
    for (std::size_t i = from + 1; i < to; ++i)
        worst = std::max(worst, std::fabs(x[i] - x[i - 1]));
    return worst;
}
} // namespace

TEST_CASE("voice.trigger: a trigger at frame 37 starts at output frame 37", "[voice][trigger]")
{
    auto data = ramp(500, 0.1f, 0.001f);
    TestBuffer b(data);
    TrackVoice v;
    v.prepare(kRate, 64);
    v.setSample(b.get());
    const auto out = run(v, 3, [&](int block) {
        if (block == 1)
            v.start(params(), 0.0f, 37, 1.0f);
    });
    for (int i = 0; i < 64 + 37; ++i)
        REQUIRE(out.mainL[static_cast<std::size_t>(i)] == 0.0f);
    for (int i = 0; i < 64 + 27; ++i)
    {
        REQUIRE(out.mainL[static_cast<std::size_t>(64 + 37 + i)] == data[static_cast<std::size_t>(i)]);
        REQUIRE(out.mainR[static_cast<std::size_t>(64 + 37 + i)] == data[static_cast<std::size_t>(i)]); // mono → L = R
    }
}

TEST_CASE("voice.trigger: a retrigger on a full-scale DC sample steps by at most 1/88 of full scale", "[voice][trigger]")
{
    TestBuffer b(std::vector<float>(44100, 1.0f));
    TrackVoice v;
    v.prepare(kRate, 64);
    REQUIRE(v.fadeFrames() == 88);
    v.setSample(b.get());
    const int retriggerAt = 5 * 64 + 20;
    const auto out = run(v, 12, [&](int block) {
        if (block == 0)
            v.start(params(), 0.0f, 0, 1.0f);
        if (block == retriggerAt / 64)
            v.start(params(), 0.0f, retriggerAt % 64, 1.0f); // envelope gain 1 at the trigger frame
    });
    // The old note: the main voice up to the trigger, then its tail.
    std::vector<float> old(out.size());
    for (std::size_t i = 0; i < out.size(); ++i)
        old[i] = static_cast<int>(i) < retriggerAt ? out.mainL[i] : out.tailL[i];
    const float step = maxStep(old, 1, out.size());
    INFO("largest step of the old note " << step << " (1/88 = " << 1.0f / 88 << ")");
    CHECK(step <= 1.0f / 88.0f + 1e-6f);
    CHECK(old[static_cast<std::size_t>(retriggerAt)] > 0.98f);                  // the tail starts at the old level
    CHECK(old[static_cast<std::size_t>(retriggerAt + 87)] == 0.0f);             // … and is gone after 2 ms
    CHECK(out.mainL[static_cast<std::size_t>(retriggerAt)] == 1.0f);            // the new note starts at its frame
    CHECK(out.mainL[static_cast<std::size_t>(retriggerAt - 1)] == 1.0f);
    for (std::size_t i = static_cast<std::size_t>(retriggerAt + 88); i < out.size(); ++i)
        REQUIRE(out.tailL[i] == 0.0f);
    CHECK(b.refs() == 2); // track + sounding note; the tail released its reference
}

TEST_CASE("voice.trigger: the tail keeps the envelope gain of the old note", "[voice][trigger]")
{
    TestBuffer b(std::vector<float>(10000, 1.0f));
    TrackVoice v;
    v.prepare(kRate, 64);
    v.setSample(b.get());
    const auto out = run(v, 4, [&](int block) {
        if (block == 0)
            v.start(params(), 0.0f, 0, 1.0f);
        if (block == 1)
            v.start(params(), 0.0f, 10, 0.5f);
    });
    CHECK_THAT(static_cast<double>(out.tailL[64 + 10]), WithinAbs(0.5 * 87.0 / 88.0, 1e-6));
    CHECK(maxStep(out.tailL, 64 + 10, out.size()) <= 0.5f / 88.0f + 1e-6f);
}

TEST_CASE("voice.trigger: a third trigger inside 2 ms cuts the older tail with a 64-frame fade", "[voice][trigger]")
{
    TestBuffer b(std::vector<float>(10000, 1.0f));
    TrackVoice v;
    v.prepare(kRate, 64);
    v.setSample(b.get());
    const auto out = run(v, 6, [&](int block) {
        if (block == 0)
            v.start(params(), 0.0f, 0, 1.0f);
        if (block == 1)
        {
            v.start(params(), 0.0f, 0, 1.0f);  // note 1 → tail (2 ms)
            v.start(params(), 0.0f, 10, 1.0f); // inside it: that tail is cut over 64 frames, note 2 → tail
        }
    });
    // From the second hand-off on, the tails only fade: at most 1/88 (the new tail) + 1/72 (the cut, 78/88 over 64).
    CHECK(maxStep(out.tailL, 64 + 11, out.size()) <= 1.0f / 88.0f + 1.0f / 64.0f);
    CHECK(out.tailL[64 + 10 + 64] < out.tailL[64 + 10 + 63]);
    for (std::size_t i = 64 + 10 + 88; i < out.size(); ++i)
        REQUIRE(out.tailL[i] == 0.0f);
    CHECK(out.mainL.back() == 1.0f);
    CHECK(b.refs() == 2);
}

TEST_CASE("voice.trigger: stop fades out in 2 ms (choke), a custom fade takes its length", "[voice][trigger]")
{
    TestBuffer b(std::vector<float>(10000, 1.0f));
    TrackVoice v;
    v.prepare(kRate, 64);
    v.setSample(b.get());
    const auto out = run(v, 6, [&](int block) {
        if (block == 0)
            v.start(params(), 0.0f, 0, 1.0f);
        if (block == 2)
            v.stop(16);
    });
    CHECK(out.mainL[128 + 15] == 1.0f);
    CHECK(maxStep(out.mainL, 1, out.size()) <= 1.0f / 88.0f + 1e-6f);
    CHECK(out.mainL[128 + 16 + 87] == 0.0f);
    CHECK_FALSE(v.active());
    CHECK_FALSE(v.ended()); // stopped, not finished

    const auto quick = run(v, 3, [&](int block) {
        if (block == 0)
        {
            v.start(params(), 0.0f, 0, 1.0f);
            v.stop(10, 0.5f); // 22 frames at 44.1 kHz
        }
    });
    CHECK(quick.mainL[9] == 1.0f);
    CHECK(quick.mainL[10 + 21] == 0.0f);
    CHECK(quick.mainL[10 + 20] > 0.0f);
}

TEST_CASE("voice.trigger: bypass silences the track and keeps its settings", "[voice][trigger]")
{
    TestBuffer b(std::vector<float>(10000, 1.0f));
    TrackVoice v;
    v.prepare(kRate, 64);
    v.setSample(b.get());
    auto p = params();
    const auto out = run(v, 6, [&](int block) {
        if (block == 0)
            v.start(p, 0.0f, 0, 1.0f);
        if (block == 2)
        {
            p.bypass = true;
            v.update(p);
        }
        if (block == 4)
            v.start(p, 0.0f, 0, 1.0f); // a bypassed track starts nothing
    });
    CHECK(out.mainL[127] == 1.0f);
    CHECK(maxStep(out.mainL, 1, out.size()) <= 1.0f / 88.0f + 1e-6f);
    for (std::size_t i = 128 + 88; i < out.size(); ++i)
        REQUIRE(out.mainL[i] == 0.0f);
    p.bypass = false;
    v.update(p);
    const auto again = run(v, 1, [&](int) { v.start(p, 0.0f, 0, 1.0f); });
    CHECK(again.mainL[0] == 1.0f);
}

TEST_CASE("voice.trigger: a sample swap fades the old note and plays later triggers from the new buffer",
          "[voice][trigger]")
{
    TestBuffer a(std::vector<float>(10000, 1.0f));
    TestBuffer c(std::vector<float>(10000, -0.5f));
    {
        TrackVoice v;
        v.prepare(kRate, 64);
        const auto out = run(v, 8, [&](int block) {
            if (block == 0)
            {
                v.setSample(a.get());
                v.start(params(), 0.0f, 0, 1.0f);
            }
            if (block == 2)
                v.setSample(c.get());
            if (block == 5)
                v.start(params(), 0.0f, 0, 1.0f);
        });
        CHECK(maxStep(out.mainL, 1, 5 * 64) <= 1.0f / 88.0f + 1e-6f);
        CHECK(out.mainL[128 + 88] == 0.0f);
        CHECK(out.mainL[5 * 64] == -0.5f);
        CHECK(a.refs() == 0); // the faded note released the old buffer, the track holds the new one
        CHECK(c.refs() == 2);
        CHECK(v.sample() == c.get());
    }
    CHECK(c.refs() == 0); // the voice's destructor drops everything
}

TEST_CASE("voice.trigger: pitch modulation sources add in cents, applied per block", "[voice][trigger]")
{
    TestBuffer b(ramp(20000, 0.0f, 1.0f / 20000.0f));
    TrackVoice v;
    v.prepare(kRate, 64);
    v.setSample(b.get());
    run(v, 1, [&](int) {
        v.setModCents(static_cast<int>(engine::voice::ModSource::Transpose), 700.0f);
        v.setModCents(static_cast<int>(engine::voice::ModSource::Lfo), -200.0f);
        v.start(params(0, engine::voice::kWholeSample, false, 0, false, 300.0f), 400.0f, 0, 1.0f); // + note +400
    });
    // 300 + 400 + 700 − 200 = 1200 cents: one octave, two source frames per output frame.
    CHECK(v.position() == 128);
    v.setModCents(static_cast<int>(engine::voice::ModSource::Lfo), -1400.0f); // now 0 cents
    run(v, 1);
    CHECK(v.position() == 128 + 64);
}

TEST_CASE("voice.trigger: events inside one block apply in frame order", "[voice][trigger]")
{
    TestBuffer b(ramp(5000, 0.0f, 0.0001f));
    TrackVoice v;
    v.prepare(kRate, 64);
    v.setSample(b.get());
    const auto out = run(v, 2, [&](int block) {
        if (block == 0)
        {
            v.start(params(1000), 0.0f, 40, 1.0f); // queued out of order
            v.start(params(0), 0.0f, 10, 1.0f);
        }
    });
    CHECK(out.mainL[9] == 0.0f);
    CHECK(out.mainL[10] == b.at(0, 0));
    CHECK(out.mainL[39] == b.at(0, 29));
    CHECK(out.mainL[40] == b.at(0, 1000));
}

TEST_CASE("voice.rt: nothing allocates after prepare", "[voice][rt]")
{
    TestBuffer b(sine(30000, 440.0), sine(30000, 660.0));
    TestBuffer c(sine(30000, 220.0));
    TrackVoice v;
    v.prepare(kRate, 64);
    engine::voice::PreviewVoice pv;
    pv.prepare(kRate);
    std::vector<float> l(64), r(64), tl(64), tr(64);
    {
        core::rt::ScopedRealtimeThread realtime; // APP_RT_CHECKS: any allocation from here on aborts
        for (int block = 0; block < 400; ++block)
        {
            v.setSample(block % 100 < 50 ? b.get() : c.get());
            v.setQuality(block % 3 == 0 ? engine::voice::InterpolatorKind::Sinc16
                                        : engine::voice::InterpolatorKind::Hermite4);
            v.setModCents(static_cast<int>(engine::voice::ModSource::Lfo), static_cast<float>(block % 50) * 10.0f);
            if (block % 3 == 0)
                v.start(params(block, 5000, block % 2 == 0, block + 100, block % 4 == 0, 100.0f), 0.0f, block % 64,
                        0.8f);
            if (block % 7 == 0)
                v.stop(block % 64);
            pv.install(block % 40 < 20 ? c.get() : b.get());
            pv.setGain(0.5f);
            v.render(l.data(), r.data(), tl.data(), tr.data());
            pv.render(tl.data(), tr.data());
        }
    }
    CHECK(core::rt::violationCount() == 0);
}
