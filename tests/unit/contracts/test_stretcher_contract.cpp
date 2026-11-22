// test_stretcher_contract: every TimeStretcher in StretcherRegistry::builtin() — the own backends "pvoc" and
// "wsola", and "none" — keeps the TimeStretcher contract:
//   - consumption: Σ framesNeeded(n) / Σ n → 1 / timeRatio (steady state within 0.1 %), for time ratios 0.5–2
//   - length: a finite input comes out timeRatio times as long, within 1 %
//   - pitch: a 440 Hz sine stays at 440 Hz ± 1 Hz at every time ratio, and moves to 440·pitchScale ± 1 Hz
//     ("none" resamples: its pitch follows the speed, 440 / timeRatio)
//   - latency: an onset at input frame k sounds at output frame k·timeRatio + latencySamples() (± 10 ms)
//   - level: a steady sine keeps its RMS within 1 dB; no NaN/Inf; reset() returns to silence
//   - real time: framesNeeded() and process() never allocate (run on a real-time-marked thread)
// The test fake "contract-resample" (tests/fakes/FakeStretcher.h, registered by test_registries.cpp in this executable)
// uses its own overlapping-input convention ("the caller advances its input by framesNeeded − 1"), which differs
// from the header's "consumes inFrames == framesNeeded(outFrames)"; it is not a backend and is skipped here.
#include "engine/voice/TimeStretcher.h"
#include "tests/unit/stretch/StretchHarness.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>
#include <string_view>

namespace
{
using namespace stretchtest;
using engine::voice::StretcherRegistry;

bool isFake(std::string_view name) { return name.rfind("contract-", 0) == 0; }
bool preservesPitch(std::string_view name) { return name != "none"; }

template <typename F> void forEachBackend(F&& f)
{
    int n = 0;
    for (const auto& e : StretcherRegistry::builtin().all())
    {
        if (isFake(e.name))
            continue;
        INFO("backend " << std::string(e.name));
        auto s = e.create();
        REQUIRE(s != nullptr);
        s->prepare(kRate, 64, 2);
        f(std::string(e.name), *s);
        ++n;
    }
    CHECK(n >= 3); // pvoc, wsola, none
}
} // namespace

TEST_CASE("test_stretcher_contract: the own backends and none are registered", "[contracts][stretch]")
{
    for (const char* name : {"pvoc", "wsola", "none"})
    {
        INFO(name);
        CHECK(StretcherRegistry::builtin().find(name) != nullptr);
    }
}

TEST_CASE("test_stretcher_contract: consumption follows the time ratio", "[contracts][stretch]")
{
    forEachBackend([](const std::string&, engine::voice::TimeStretcher& s) {
        const auto in = sine(200000, 440.0);
        for (const double t : {0.5, 0.8, 1.0, 1.25, 2.0})
        {
            INFO("timeRatio " << t);
            s.setRatios(t, 1.0);
            s.reset();
            const int out = 44100;
            const auto o = run(s, in, in, out);
            CHECK_FALSE(o.allocated);
            // Steady state: between output blocks 200 and the last.
            const long c0 = o.consumedAt[200], c1 = o.consumedAt.back();
            const double outSpan = static_cast<double>(out - 201 * 64);
            const double rate = static_cast<double>(c1 - c0) / outSpan; // input frames per output frame
            CHECK(std::fabs(rate * t - 1.0) < 1.0e-3);
        }
    });
}

TEST_CASE("test_stretcher_contract: length within 1 % for time ratios 0.5-2", "[contracts][stretch]")
{
    forEachBackend([](const std::string&, engine::voice::TimeStretcher& s) {
        const int frames = 88200; // 2 s of 440 Hz, then silence
        const auto in = sine(frames, 440.0);
        for (const double t : {0.5, 0.8, 1.25, 2.0})
        {
            INFO("timeRatio " << t);
            s.setRatios(t, 1.0);
            s.reset();
            const int out = static_cast<int>(frames * t) + 22050;
            const auto o = run(s, in, in, out);
            CHECK_FALSE(o.allocated);
            const long end = endOf(o.l, 0.5 * 0.5 / std::sqrt(2.0)); // half the steady RMS
            const double expected = frames * t;
            INFO("end " << end << " expected " << expected);
            CHECK(std::fabs(static_cast<double>(end) - expected) <= 0.01 * expected);
        }
    });
}

TEST_CASE("test_stretcher_contract: 440 Hz stays 440 Hz +-1 Hz, pitch scale moves it", "[contracts][stretch]")
{
    forEachBackend([](const std::string& name, engine::voice::TimeStretcher& s) {
        const auto in = sine(250000, 440.0);
        struct Case
        {
            double t, p;
        };
        for (const Case c : {Case {0.5, 1.0}, Case {0.8, 1.0}, Case {1.0, 1.0}, Case {1.25, 1.0}, Case {2.0, 1.0},
                             Case {1.0, std::exp2(7.0 / 12.0)}, Case {1.25, std::exp2(7.0 / 12.0)},
                             Case {0.8, std::exp2(-12.0 / 12.0)}, Case {2.0, std::exp2(12.0 / 12.0)}})
        {
            INFO("timeRatio " << c.t << " pitch " << c.p);
            s.setRatios(c.t, c.p);
            s.reset();
            const auto o = run(s, in, in, 66150);
            CHECK_FALSE(o.allocated);
            CHECK(finite(o.l));
            const double expected = preservesPitch(name) ? 440.0 * c.p : 440.0 / c.t;
            const double f = frequency(o.l, 11025, 66150);
            INFO("measured " << f << " expected " << expected);
            CHECK(std::fabs(f - expected) <= 1.0);
            const double fr = frequency(o.r, 11025, 66150);
            CHECK(std::fabs(fr - expected) <= 1.0);
        }
    });
}

TEST_CASE("test_stretcher_contract: the onset sounds at the declared latency", "[contracts][stretch]")
{
    forEachBackend([](const std::string&, engine::voice::TimeStretcher& s) {
        const int start = 22050;
        const auto in = sine(start + 66150, 440.0, 0.5f, start);
        for (const double t : {0.5, 1.0, 2.0})
        {
            INFO("timeRatio " << t);
            s.setRatios(t, 1.0);
            s.reset();
            const auto o = run(s, in, in, static_cast<int>(start * t) + 22050);
            const long on = onset(o.l, 0.5 * 0.5 / std::sqrt(2.0));
            const double expected = start * t + s.latencySamples();
            INFO("onset " << on << " expected " << expected);
            CHECK(std::fabs(static_cast<double>(on) - expected) <= 441.0);
        }
    });
}

TEST_CASE("test_stretcher_contract: level kept within 1 dB, reset returns to silence", "[contracts][stretch]")
{
    forEachBackend([](const std::string&, engine::voice::TimeStretcher& s) {
        const auto in = sine(200000, 440.0);
        const double ref = 0.5 / std::sqrt(2.0);
        for (const double t : {0.5, 1.0, 2.0})
        {
            INFO("timeRatio " << t);
            s.setRatios(t, 1.0);
            s.reset();
            const auto o = run(s, in, in, 44100);
            const double db = 20.0 * std::log10(rms(o.l, 11025, 44100) / ref);
            INFO("level " << db << " dB");
            CHECK(std::fabs(db) <= 1.0);
        }
        s.reset();
        const std::vector<float> silence;
        const auto o = run(s, silence, silence, 8192);
        CHECK(rms(o.l, 0, 8192) == 0.0);
        CHECK(rms(o.r, 0, 8192) == 0.0);
    });
}

TEST_CASE("test_stretcher_contract: ratio changes every block stay finite and in step", "[contracts][stretch]")
{
    forEachBackend([](const std::string&, engine::voice::TimeStretcher& s) {
        const auto in = sine(400000, 330.0);
        s.setRatios(1.0, 1.0);
        s.reset();
        double expected = 0.0;
        double t = 1.0;
        const auto o = run(s, in, in, 88200, 64, [&](int b, engine::voice::TimeStretcher& st) {
            t = 0.5 + 1.5 * (0.5 + 0.5 * std::sin(b * 0.05)); // 0.5…2, a slow sweep
            const double p = std::exp2((b % 97 - 48) / 48.0);  // −12…+12 st
            st.setRatios(t, p);
            expected += 64.0 / t;
        });
        CHECK_FALSE(o.allocated);
        CHECK(finite(o.l));
        CHECK(finite(o.r));
        CHECK(rms(o.l, 22050, 88200) > 0.1);
        // Total consumption against Σ n / t: the only difference is the look-ahead, which depends on the current ratio
        // (≤ W/2·(1 + 1/R) + search + one analysis hop, a few thousand frames; 8192 bounds it for every backend).
        CHECK(std::fabs(static_cast<double>(o.consumed) - expected) <= 8192.0);
    });
}
