// The track LFO (spec 02-features/lfo.md LFO-001…011): shapes against their analytic values,
// the speed laws, the SYNC phase locked to the transport PPQ, the seeded Random sequence, restarts, one-shot and the
// 50 Hz block smoother.
#include "core/util/lfo/LfoLaw.h"
#include "engine/modulation/Lfo.h"
#include "engine/modulation/Smoothers.h"
#include "engine/transport/Transport.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

using engine::modulation::Lfo;
using engine::modulation::LfoShape;
using engine::modulation::lfoShapeValue;

namespace
{
constexpr double kTwoPi = 6.283185307179586476925286766559;

double analytic(LfoShape s, double p)
{
    switch (s)
    {
        case LfoShape::Sine: return std::sin(kTwoPi * p);
        case LfoShape::Triangle: return p < 0.25 ? 4 * p : (p < 0.75 ? 2 - 4 * p : 4 * p - 4);
        case LfoShape::Saw: return 1 - 2 * p;
        case LfoShape::Pulse50: return p < 0.5 ? 1 : -1;
        case LfoShape::Pulse25: return p < 0.25 ? 1 : -1;
        case LfoShape::Pulse12: return p < 0.125 ? 1 : -1;
        case LfoShape::Random: break;
    }
    return 0;
}

// An LFO with no smoothing effect on the comparison: the raw value is checked.
Lfo makeLfo(LfoShape shape, double fs = 44100.0)
{
    Lfo l;
    l.prepare(fs);
    l.setShape(shape);
    l.setDepth(1.0f);
    return l;
}
} // namespace

TEST_CASE("lfo.shapes: every shape matches its analytic value at 1000 phases (<= 1e-6)", "[lfo]")
{
    for (const auto s : {LfoShape::Sine, LfoShape::Triangle, LfoShape::Saw, LfoShape::Pulse50, LfoShape::Pulse25,
                         LfoShape::Pulse12})
    {
        INFO("shape " << static_cast<int>(s));
        double worst = 0;
        for (int i = 0; i < 1000; ++i)
        {
            const double p = (i + 0.37) / 1000.0; // off the pulse edges
            worst = std::fmax(worst, std::fabs(static_cast<double>(lfoShapeValue(s, p)) - analytic(s, p)));
            worst = std::fmax(worst, std::fabs(static_cast<double>(lfoShapeValue(s, p + 3.0)) - analytic(s, p))); // reduction
        }
        CHECK(worst <= 1e-6);
    }
    // The block evaluation is the same function: a free LFO at 1 Hz, block by block, raw values.
    Lfo l = makeLfo(LfoShape::Sine);
    l.setSpeedHz(1.0f);
    double worst = 0;
    for (int b = 0; b < 1000; ++b)
    {
        const float v = l.advanceBlock(120.0, 0.0, false);
        (void) v;
        const double expected = std::sin(kTwoPi * std::fmod(b * 64.0 / 44100.0, 1.0));
        worst = std::fmax(worst, std::fabs(static_cast<double>(l.rawValue()) - expected));
    }
    CHECK(worst <= 1e-6);
    CHECK(std::fabs(engine::modulation::sinTurns(0.25) - 1.0f) <= 2e-7f);
    CHECK(engine::modulation::sinTurns(0.0) == 0.0f);
}

TEST_CASE("lfo.laws: speed knob 0.01-40 Hz exponential, 17 sync divisions, depth and phase", "[lfo]")
{
    CHECK(std::fabs(core::lfo::speedHz(0) - 0.01f) < 1e-7f);
    CHECK(std::fabs(core::lfo::speedHz(127) - 40.0f) < 1e-4f);
    CHECK(std::fabs(core::lfo::speedHz(63.5f) - std::sqrt(0.01f * 40.0f)) < 1e-5f); // geometric middle
    for (int v = 1; v <= 127; ++v)
        CHECK(core::lfo::speedHz(static_cast<float>(v)) > core::lfo::speedHz(static_cast<float>(v - 1)));
    CHECK(core::lfo::syncDivisionIndex(0) == 0);
    CHECK(core::lfo::syncDivisionIndex(127) == 16);
    for (int i = 0; i < core::lfo::kNumSyncDivisions; ++i)
    {
        CHECK(core::lfo::syncDivisionIndex(core::lfo::syncDivisionKnob(i)) == i);
        if (i > 0)
            CHECK(core::lfo::kSyncDivisions[static_cast<std::size_t>(i)].quarters <
                  core::lfo::kSyncDivisions[static_cast<std::size_t>(i - 1)].quarters);
    }
    CHECK(core::lfo::kSyncDivisions[0].quarters == 64.0);   // 16 bars
    CHECK(core::lfo::kSyncDivisions[15].quarters == 0.125); // 1/32
    CHECK(core::lfo::depth01(127) == 1.0f);
    CHECK(core::lfo::depth01(0) == 0.0f);
    CHECK(core::lfo::phase01(64) == 0.5f);
    CHECK(core::lfo::phase01(127) < 1.0f);
}

TEST_CASE("lfo.free: the free phase follows hz · 64 / fs per block, from the phase offset", "[lfo]")
{
    Lfo l = makeLfo(LfoShape::Saw, 48000.0);
    l.setSpeedHz(7.3f);
    l.setPhaseOffset(0.25f);
    const int blocks = 100000;
    for (int b = 0; b < blocks; ++b)
        l.advanceBlock(120.0, 0.0, false);
    const double expected = std::fmod(0.25 + (blocks - 1) * 64.0 * static_cast<double>(7.3f) / 48000.0, 1.0);
    CHECK(std::fabs(l.phase() - expected) < 1e-7); // ~1000 cycles accumulated in double
    // Clamped speed range.
    l.setSpeedHz(1000.0f);
    l.restart();
    l.advanceBlock(120.0, 0.0, false);
    l.advanceBlock(120.0, 0.0, false);
    CHECK(std::fabs(l.phase() - std::fmod(0.25 + 40.0 * 64.0 / 48000.0, 1.0)) < 1e-9);
}

TEST_CASE("lfo.sync: phase locked to the PPQ after 1000 bars at 133.37 BPM (error <= 1 sample)", "[lfo]")
{
    constexpr double fs = 44100.0;
    const double quarters = core::lfo::kSyncDivisions[10].quarters; // 1/8: 0.5 quarter per cycle
    // Error of the LFO phase against the closed form from the sample count, in samples of the cycle.
    auto errorSamples = [&](const Lfo& l, std::int64_t frames, double bpm) {
        const double samplesPerQuarter = fs * 60.0 / bpm;
        const double cycles = static_cast<double>(frames) / (samplesPerQuarter * quarters);
        double d = std::fabs(std::fmod(0.125 + cycles, 1.0) - l.phase());
        d = std::fmin(d, 1.0 - d);
        return d * quarters * samplesPerQuarter;
    };

    // 1. 133.37 BPM exactly (off the transport's 0.1 BPM grid): a PPQ accumulated block by block, as a transport does.
    {
        constexpr double bpm = 133.37;
        Lfo l = makeLfo(LfoShape::Sine, fs);
        l.setSyncDivision(10);
        l.setPhaseOffset(0.125f);
        l.restartAt(0.0);
        const double ppqPerBlock = 64.0 * bpm / (60.0 * fs);
        double ppq = 0, worst = 0;
        std::int64_t frames = 0;
        for (; ppq < 4000.0; ppq += ppqPerBlock, frames += 64) // 1000 bars of 4/4
        {
            l.advanceBlock(bpm, ppq, true);
            if ((frames & 0xFFFF) == 0)
                worst = std::fmax(worst, errorSamples(l, frames, bpm));
        }
        l.advanceBlock(bpm, ppq, true);
        worst = std::fmax(worst, errorSamples(l, frames, bpm));
        INFO("worst phase error " << worst << " samples");
        CHECK(worst <= 1.0);
        CHECK(frames > 79'000'000);
    }

    // 2. The engine's transport (its tempo grid is 0.1 BPM: 133.37 plays as 133.4), every block checked.
    engine::transport::Transport t;
    t.prepare(fs);
    t.setTempo(133.37);
    t.command({engine::TransportCmd::Play, 0});
    Lfo l = makeLfo(LfoShape::Sine, fs);
    l.setSyncDivision(10);
    l.setPhaseOffset(0.125f);
    engine::BlockContext c {};
    c.sampleRate = fs;
    c.frames = engine::kBlockFrames;
    double worst = 0;
    std::int64_t frames = 0;
    for (bool first = true;; first = false, frames += engine::kBlockFrames)
    {
        t.advance(c);
        if (first)
            l.restartAt(c.ppqStart); // transport start
        l.advanceBlock(c.bpm, c.ppqStart, c.playing);
        worst = std::fmax(worst, errorSamples(l, frames, c.bpm));
        if (c.ppqStart >= 4000.0)
            break;
    }
    INFO("transport tempo " << c.bpm << ", worst phase error " << worst << " samples");
    CHECK(worst <= 1.0);

    // A tempo change keeps the lock (the phase is a function of the PPQ, not of a rate).
    t.setTempo(97.1);
    for (int b = 0; b < 100000; ++b)
    {
        t.advance(c);
        l.advanceBlock(c.bpm, c.ppqStart, c.playing);
    }
    const double expected = std::fmod(0.125 + c.ppqStart / quarters, 1.0);
    double d = std::fabs(expected - l.phase());
    CHECK(std::fmin(d, 1.0 - d) * quarters * fs * 60.0 / c.bpm <= 1.0);
}

TEST_CASE("lfo.sync: stopped runs at the tempo, start re-anchors, switches keep the phase continuous", "[lfo]")
{
    constexpr double fs = 44100.0;
    Lfo l = makeLfo(LfoShape::Saw, fs);
    l.setSyncDivision(7); // 1/4: one cycle per quarter
    l.advanceBlock(120.0, 0.0, false);
    l.advanceBlock(120.0, 0.0, false);
    // Stopped: 120 BPM = 2 quarters per second → 2 · 64 / 44100 cycles per block.
    CHECK(std::fabs(l.phase() - 2.0 * 64.0 / fs) < 1e-12);
    // Transport running from ppq 8: the phase continues there (no jump) and then follows the PPQ.
    const double before = l.phase() + 2.0 * 64.0 / fs;
    l.advanceBlock(120.0, 8.0, true);
    CHECK(std::fabs(l.phase() - before) < 1e-12);
    l.advanceBlock(120.0, 8.25, true);
    CHECK(std::fabs(l.phase() - std::fmod(before + 0.25, 1.0)) < 1e-12);
    // Another division: continuous at that block, then locked at the new length.
    l.setSyncDivision(5); // 1/2: two quarters per cycle
    const double at = l.phase();
    l.advanceBlock(120.0, 8.25, true);
    CHECK(std::fabs(l.phase() - at) < 1e-12);
    l.advanceBlock(120.0, 9.25, true);
    CHECK(std::fabs(l.phase() - std::fmod(at + 0.5, 1.0)) < 1e-12);
    // An exact restart in this block shows from the next block: anchored at its PPQ.
    l.restartAt(9.30);
    l.advanceBlock(120.0, 9.25, true);
    CHECK(l.elapsed() == 0.0);
    l.advanceBlock(120.0, 9.80, true);
    CHECK(std::fabs(l.elapsed() - 0.25) < 1e-12);
}

TEST_CASE("lfo.random: the same seed renders the same sequence, restarts repeat it (LFO-008)", "[lfo]")
{
    auto run = [](std::uint32_t seed, int blocks) {
        Lfo l = makeLfo(LfoShape::Random);
        l.setSeed(seed);
        l.setSpeedHz(17.0f);
        std::vector<float> v;
        for (int b = 0; b < blocks; ++b)
            v.push_back(l.advanceBlock(120.0, 0.0, false));
        return v;
    };
    const auto a = run(core::lfo::seedOf(5), 20000), b = run(core::lfo::seedOf(5), 20000),
               c = run(core::lfo::seedOf(6), 20000);
    CHECK(a == b);
    CHECK(a != c);
    // Sample and hold: one new value per cycle (17 Hz · 20000 blocks · 64 / 44100 ≈ 493 cycles), within [−1, 1].
    int changes = 0;
    float lo = 1, hi = -1;
    for (std::size_t i = 1; i < a.size(); ++i)
    {
        changes += a[i] != a[i - 1];
        lo = std::fmin(lo, a[i]);
        hi = std::fmax(hi, a[i]);
    }
    CHECK(changes >= 480);
    CHECK(changes <= 494);
    CHECK(lo >= -1.0f);
    CHECK(hi <= 1.0f);
    CHECK(hi - lo > 1.5f);
    // A restart replays the sequence from its first value.
    Lfo l = makeLfo(LfoShape::Random);
    l.setSeed(core::lfo::seedOf(5));
    l.setSpeedHz(17.0f);
    std::vector<float> first, again;
    for (int k = 0; k < 2000; ++k)
        first.push_back(l.advanceBlock(120.0, 0.0, false));
    l.restart();
    for (int k = 0; k < 2000; ++k)
        again.push_back(l.advanceBlock(120.0, 0.0, false));
    CHECK(first == again);
    // Neighbouring Random Start numbers give different, non-zero seeds.
    for (int n = 0; n < 127; ++n)
    {
        CHECK(core::lfo::seedOf(n) != 0u);
        CHECK(core::lfo::seedOf(n) != core::lfo::seedOf(n + 1));
    }
}

TEST_CASE("lfo.oneshot: one cycle from the restart, then the cycle's end value is held", "[lfo]")
{
    Lfo l = makeLfo(LfoShape::Saw);
    l.setOneShot(true);
    l.setSpeedHz(10.0f); // 4410 samples per cycle ≈ 69 blocks
    float last = 0;
    for (int b = 0; b < 200; ++b)
        last = l.advanceBlock(120.0, 0.0, false);
    CHECK(l.rawValue() < -0.9999f); // the falling saw's end
    CHECK(std::fabs(last - l.rawValue()) < 1e-6f);
    l.restart();
    l.advanceBlock(120.0, 0.0, false);
    CHECK(l.rawValue() == 1.0f); // the cycle starts again
}

TEST_CASE("lfo.smoothing: continuous shapes pass the 50 Hz one-pole per block, pulses and Random do not", "[lfo]")
{
    constexpr double fs = 44100.0;
    const float k = static_cast<float>(1.0 - std::exp(-kTwoPi * 50.0 * 64.0 / fs));
    Lfo l = makeLfo(LfoShape::Saw, fs);
    l.setSpeedHz(0.01f);
    const float y1 = l.advanceBlock(120.0, 0.0, false); // raw ≈ +1 from a smoother at 0
    CHECK(std::fabs(y1 - k * l.rawValue()) < 1e-6f);
    const float y2 = l.advanceBlock(120.0, 0.0, false);
    CHECK(std::fabs(y2 - (y1 + k * (l.rawValue() - y1))) < 1e-6f);
    Lfo p = makeLfo(LfoShape::Pulse25, fs);
    p.setSpeedHz(3.0f);
    for (int b = 0; b < 500; ++b)
    {
        const float v = p.advanceBlock(120.0, 0.0, false);
        CHECK((v == 1.0f || v == -1.0f));
        if (v != p.rawValue())
            FAIL("pulse smoothed at block " << b);
    }
}
