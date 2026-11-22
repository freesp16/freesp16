// TIME STRETCH in the track voice (TrackVoice + engine/voice/stretch/VoiceStretch.h): RESAMPLE speed, MASTER TEMPO
// length and pitch, the onset, the region end, priming (two MASTER TEMPO starts per block), the governor's hold, the
// "none" backend, and a click-free retrigger of a stretched note. Every voice call runs on a real-time-marked thread.
#include "engine/voice/stretch/StretcherBank.h"
#include "tests/unit/voice/VoiceHarness.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <string>

using namespace voicetest;
using engine::voice::StretchMode;
using engine::voice::TrackVoice;
using engine::voice::stretch::StretcherBank;

namespace
{
engine::voice::PlaybackParams stretchParams(StretchMode mode, float cents = 0.0f, bool loop = false)
{
    auto p = params(0, engine::voice::kWholeSample, loop, 0, false, cents);
    p.stretch = mode;
    return p;
}

// Last frame whose |value| exceeds `level` (the note's audible end).
long lastAbove(const std::vector<float>& x, float level)
{
    for (std::size_t i = x.size(); i-- > 0;)
        if (std::fabs(x[i]) > level)
            return static_cast<long>(i);
    return -1;
}
long firstAbove(const std::vector<float>& x, float level)
{
    for (std::size_t i = 0; i < x.size(); ++i)
        if (std::fabs(x[i]) > level)
            return static_cast<long>(i);
    return -1;
}

struct Rig
{
    StretcherBank bank;
    TrackVoice voice;
    explicit Rig(const std::string& backend = std::string(StretcherBank::kDefaultBackend))
    {
        bank.select(backend);
        bank.prepare(kRate, kBlockFrames);
        voice.prepare(kRate, kBlockFrames);
        voice.attachStretch(&bank, 0);
    }
};
} // namespace

TEST_CASE("stretch voice: RESAMPLE plays at project / sample BPM and ignores PITCH", "[stretch][voice]")
{
    const TestBuffer buf(sine(44100, 440.0)); // 1 s
    Rig rig;
    rig.voice.setSample(buf.get());
    const auto out = run(rig.voice, 900, [&](int b) {
        rig.bank.beginBlock({});
        rig.voice.setTempo(150.0, 120.0f);
        if (b == 0)
            rig.voice.start(stretchParams(StretchMode::Resample, 700.0f), 0.0f, 0, 1.0f);
    });
    CHECK(rig.voice.playMode() == StretchMode::Resample);
    CHECK(std::fabs(frequency(out.mainL, 2000, 30000) - 550.0) <= 1.0);
    CHECK(std::fabs(lastAbove(out.mainL, 0.1f) - 35280.0) <= 64.0); // 0.8 s
}

TEST_CASE("stretch voice: MASTER TEMPO keeps the pitch and follows the tempo", "[stretch][voice]")
{
    for (const char* backend : {"wsola", "pvoc"})
    {
        INFO("backend " << backend);
        const TestBuffer buf(sine(44100, 440.0), sine(44100, 440.0));
        Rig rig(backend);
        rig.voice.setSample(buf.get());
        // Sample at 120 BPM in a 60 BPM project: twice as long, same pitch.
        const auto out = run(rig.voice, 1500, [&](int b) {
            rig.bank.beginBlock({});
            rig.voice.setTempo(60.0, 120.0f);
            if (b == 0)
                rig.voice.start(stretchParams(StretchMode::MasterTempo), 0.0f, 0, 1.0f);
        });
        CHECK(std::fabs(frequency(out.mainL, 4410, 80000) - 440.0) <= 1.0);
        CHECK(std::fabs(frequency(out.mainR, 4410, 80000) - 440.0) <= 1.0);
        const long end = lastAbove(out.mainL, 0.1f);
        INFO("end " << end);
        CHECK(std::fabs(end - 88200.0) <= 882.0); // within 1 %
        CHECK(firstAbove(out.mainL, 0.1f) < 441);  // the onset is not late (< 10 ms)
        CHECK(rig.voice.ended());
        CHECK_FALSE(rig.voice.mainActive());
    }
}

TEST_CASE("stretch voice: MASTER TEMPO pitch moves the frequency only (+-12 st)", "[stretch][voice]")
{
    const TestBuffer buf(sine(44100, 440.0));
    Rig rig("pvoc");
    rig.voice.setSample(buf.get());
    const auto out = run(rig.voice, 800, [&](int b) {
        rig.bank.beginBlock({});
        rig.voice.setTempo(120.0, 120.0f);
        if (b == 0)
            rig.voice.start(stretchParams(StretchMode::MasterTempo, 700.0f), 0.0f, 0, 1.0f);
    });
    CHECK(std::fabs(frequency(out.mainL, 4410, 40000) - 440.0 * std::exp2(7.0 / 12.0)) <= 1.0);
    CHECK(std::fabs(lastAbove(out.mainL, 0.1f) - 44100.0) <= 441.0);
    // +24 st is limited to +12 st in MASTER TEMPO (PLY-020).
    Rig rig2("pvoc");
    rig2.voice.setSample(buf.get());
    const auto out2 = run(rig2.voice, 400, [&](int b) {
        rig2.bank.beginBlock({});
        rig2.voice.setTempo(120.0, 120.0f);
        if (b == 0)
            rig2.voice.start(stretchParams(StretchMode::MasterTempo, 2400.0f), 0.0f, 0, 1.0f);
    });
    CHECK(std::fabs(frequency(out2.mainL, 4410, 20000) - 880.0) <= 1.0);
}

TEST_CASE("stretch voice: two MASTER TEMPO starts prime per block, the third waits one block", "[stretch][voice]")
{
    const TestBuffer buf(sine(44100, 440.0));
    StretcherBank bank;
    bank.prepare(kRate, kBlockFrames);
    TrackVoice v[3];
    std::vector<float> l(kBlockFrames), r(kBlockFrames), tl(kBlockFrames), tr(kBlockFrames);
    for (int i = 0; i < 3; ++i)
    {
        v[i].prepare(kRate, kBlockFrames);
        v[i].attachStretch(&bank, i);
        v[i].setSample(buf.get());
    }
    for (int b = 0; b < 2; ++b)
    {
        bank.beginBlock({});
        core::rt::ScopedRealtimeThread realtime;
        for (int i = 0; i < 3; ++i)
        {
            v[i].setTempo(120.0, 120.0f);
            if (b == 0)
                v[i].start(stretchParams(StretchMode::MasterTempo), 0.0f, 0, 1.0f);
            v[i].render(l.data(), r.data(), tl.data(), tr.data());
        }
        if (b == 0)
        {
            CHECK(v[0].stretching());
            CHECK(v[1].stretching());
            CHECK_FALSE(v[2].mainActive()); // deferred
        }
    }
    CHECK(v[2].stretching());
}

TEST_CASE("stretch voice: governor hold and the none backend play MASTER TEMPO as RESAMPLE", "[stretch][voice]")
{
    const TestBuffer buf(sine(44100, 440.0));
    {
        Rig rig;
        for (int i = 0; i < 16; ++i)
            rig.bank.countPlayer();
        rig.voice.setSample(buf.get());
        const auto out = run(rig.voice, 100, [&](int b) {
            rig.bank.beginBlock({1, 33, b == 0 ? 0 : 16}); // FX-051-class load: new MASTER TEMPO notes are held
            rig.voice.setTempo(150.0, 120.0f);
            if (b == 0)
            {
                REQUIRE(rig.bank.holdNew());
                rig.voice.start(stretchParams(StretchMode::MasterTempo), 0.0f, 0, 1.0f);
            }
        });
        CHECK(rig.voice.playMode() == StretchMode::Resample);
        CHECK_FALSE(rig.voice.stretching());
        CHECK(std::fabs(frequency(out.mainL, 1000, 6000) - 550.0) <= 2.0);
    }
    {
        Rig rig("none");
        rig.voice.setSample(buf.get());
        run(rig.voice, 4, [&](int b) {
            rig.bank.beginBlock({});
            rig.voice.setTempo(150.0, 120.0f);
            if (b == 0)
                rig.voice.start(stretchParams(StretchMode::MasterTempo), 0.0f, 0, 1.0f);
        });
        CHECK(rig.voice.playMode() == StretchMode::Resample);
    }
    {
        TrackVoice lone; // no bank at all
        lone.prepare(kRate, kBlockFrames);
        lone.setSample(buf.get());
        run(lone, 2, [&](int b) {
            lone.setTempo(150.0, 120.0f);
            if (b == 0)
                lone.start(stretchParams(StretchMode::MasterTempo), 0.0f, 0, 1.0f);
        });
        CHECK(lone.playMode() == StretchMode::Resample);
    }
}

TEST_CASE("stretch voice: unknown sample BPM uses the PLY-031 estimate, then follows the tempo", "[stretch][voice]")
{
    // 2 s = 1 bar at 120 BPM. Estimated at the 110 BPM project: 1.83 bars → 1 bar?  2 s · 110/240 = 0.92 bar → 1 bar
    // → 120 BPM. A later tempo of 60 BPM then doubles the length.
    const TestBuffer buf(sine(88200, 440.0));
    Rig rig;
    rig.voice.setSample(buf.get());
    const auto out = run(rig.voice, 2900, [&](int b) {
        rig.bank.beginBlock({});
        rig.voice.setTempo(b == 0 ? 110.0 : 60.0, 0.0f);
        if (b == 0)
            rig.voice.start(stretchParams(StretchMode::MasterTempo), 0.0f, 0, 1.0f);
    });
    CHECK(std::fabs(lastAbove(out.mainL, 0.1f) - 176400.0) <= 1764.0);
}

TEST_CASE("stretch voice: a retrigger of a stretched loop is click-free", "[stretch][voice]")
{
    const TestBuffer buf(sine(44100, 220.0, kRate, 0.5f));
    for (const char* backend : {"pvoc", "wsola"})
    {
        INFO("backend " << backend);
        Rig rig(backend);
        rig.voice.setSample(buf.get());
        const auto out = run(rig.voice, 300, [&](int b) {
            rig.bank.beginBlock({});
            rig.voice.setTempo(100.0, 120.0f);
            if (b == 0 || b == 150)
                rig.voice.start(stretchParams(StretchMode::MasterTempo, 0.0f, true), 0.0f, b == 0 ? 0 : 17, 1.0f);
        });
        // Around the retrigger the sum of main + tail moves smoothly: a 0.5 amplitude 220 Hz sine moves ≤ 0.016 per
        // frame; the old note fades over 2 ms in the stretch tail, the new one starts with the 32-frame fade-in.
        const std::size_t at = 150 * kBlockFrames + 17;
        float worst = 0.0f;
        std::size_t where = 0;
        for (std::size_t i = at - 200; i < at + 400; ++i)
            if (std::fabs(out.sum(0, i) - out.sum(0, i - 1)) > worst)
            {
                worst = std::fabs(out.sum(0, i) - out.sum(0, i - 1));
                where = i;
            }
        INFO("largest step " << worst << " at " << static_cast<long>(where) - static_cast<long>(at));
        CHECK(worst < 0.05f);
        CHECK(rig.voice.stretching());
    }
}
