// engine::transport::Metronome and the transport stage inside engine::Engine: clicks on the exact beat frames (± 0),
// accents on bar starts, pre-count clicks, the transport.* parameters through the bridge.
#include "engine/transport/Metronome.h"
#include "engine/transport/Transport.h"

#include "core/params/ParamRegistry.h"
#include "core/util/GainLaw.h"
#include "tests/unit/engine/EngineTestUtil.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

using engine::BlockContext;
using engine::TransportCmd;
using engine::transport::Metronome;
using engine::transport::Transport;

namespace
{
struct Rig
{
    Transport transport;
    Metronome metronome;
    std::vector<float> l, r;

    Rig(double rate, double bpm)
    {
        transport.prepare(rate);
        transport.setTempo(bpm);
        metronome.prepare(rate);
    }
    void run(int blocks)
    {
        l.assign(static_cast<std::size_t>(blocks) * 64, 0.0f);
        r.assign(l.size(), 0.0f);
        for (int b = 0; b < blocks; ++b)
        {
            BlockContext c {};
            c.sampleRate = transport.sampleRate();
            c.frames = 64;
            transport.advance(c);
            metronome.render(c, l.data() + b * 64, r.data() + b * 64);
        }
    }
};

// Checks that the buffer holds exactly the clicks at `frames` (accent where `accent`) and silence elsewhere.
void checkClicks(const Rig& rig, const std::vector<std::int64_t>& frames, const std::vector<bool>& accent, float gain)
{
    const int len = rig.metronome.clickFrames();
    std::vector<float> expect(rig.l.size(), 0.0f);
    for (std::size_t i = 0; i < frames.size(); ++i)
    {
        const float* table = accent[i] ? rig.metronome.accentTable() : rig.metronome.beatTable();
        for (int j = 0; j < len && frames[i] + j < static_cast<std::int64_t>(expect.size()); ++j)
            expect[static_cast<std::size_t>(frames[i] + j)] = table[j] * gain;
    }
    std::int64_t mismatches = 0, first = -1;
    for (std::size_t i = 0; i < expect.size(); ++i)
        if (rig.l[i] != expect[i] || rig.r[i] != expect[i])
        {
            if (first < 0)
                first = static_cast<std::int64_t>(i);
            ++mismatches;
        }
    INFO("first mismatch at frame " << first);
    CHECK(mismatches == 0);
}
} // namespace

TEST_CASE("transport.metronome: the clicks are 12 ms decaying sines, accent 1.5 kHz, beat 1 kHz", "[transport]")
{
    Metronome m;
    m.prepare(44100.0);
    CHECK(m.clickFrames() == 529);
    float peakA = 0, peakB = 0;
    int zeroA = 0, zeroB = 0;
    for (int i = 1; i < m.clickFrames(); ++i)
    {
        peakA = std::fmax(peakA, std::fabs(m.accentTable()[i]));
        peakB = std::fmax(peakB, std::fabs(m.beatTable()[i]));
        zeroA += (m.accentTable()[i - 1] < 0) != (m.accentTable()[i] < 0);
        zeroB += (m.beatTable()[i - 1] < 0) != (m.beatTable()[i] < 0);
    }
    CHECK(m.accentTable()[0] == 0.0f); // starts at 0: no step
    CHECK(std::fabs(m.accentTable()[m.clickFrames() - 1]) < 1e-3f);
    CHECK(peakA > 0.8f);
    CHECK(peakB < peakA);
    CHECK(zeroA == 35); // 1.5 kHz: a crossing every 1/3000 s within 528 / 44100 s
    CHECK(zeroB == 23); // 1 kHz: every 1/2000 s
    m.prepare(96000.0);
    CHECK(m.clickFrames() == Metronome::kMaxClickFrames);
}

TEST_CASE("transport.metronome: clicks fall on the beat frames ± 0 at 120 BPM, accents on bar starts", "[transport]")
{
    Rig rig(44100.0, 120.0);
    rig.metronome.set(true, 1.0f, false);
    rig.transport.command({TransportCmd::Play, 0});
    rig.run(9 * 22050 / 64 + 20);
    std::vector<std::int64_t> frames;
    std::vector<bool> accent;
    for (int k = 0; k <= 9; ++k)
    {
        frames.push_back(static_cast<std::int64_t>(k) * 22050);
        accent.push_back(k % 4 == 0);
    }
    checkClicks(rig, frames, accent, 1.0f);
}

TEST_CASE("transport.metronome: clicks fall on the beat frames ± 0 at 133.7 BPM, 48 kHz", "[transport]")
{
    Rig rig(48000.0, 133.7);
    rig.metronome.set(true, 0.5f, false);
    rig.transport.command({TransportCmd::Play, 0});
    const int blocks = 40 * 21541 / 64; // ≈ 40 beats
    rig.run(blocks);
    std::vector<std::int64_t> frames;
    std::vector<bool> accent;
    for (std::int64_t k = 0;; ++k)
    {
        const std::int64_t f = (k * 28800000 + 1336) / 1337; // ⌈k · 60 · 48000 / 133.7⌉, exact
        if (f >= static_cast<std::int64_t>(blocks) * 64)
            break;
        frames.push_back(f);
        accent.push_back(k % 4 == 0);
    }
    CHECK(frames.size() >= 39);
    checkClicks(rig, frames, accent, 0.5f);
}

TEST_CASE("transport.metronome: the pre-count clicks with the metronome off, the pattern stays silent", "[transport]")
{
    Rig rig(44100.0, 120.0);
    rig.metronome.set(true, 1.0f, true); // only the pre-count
    rig.transport.command({TransportCmd::Play, 2});
    rig.run(16 * 22050 / 64);
    std::vector<std::int64_t> frames;
    std::vector<bool> accent;
    for (int k = 0; k < 8; ++k) // beats −8 … −1: frames 0, 22050, …; bar starts at −8 and −4
    {
        frames.push_back(static_cast<std::int64_t>(k) * 22050);
        accent.push_back(k % 4 == 0);
    }
    checkClicks(rig, frames, accent, 1.0f);
}

TEST_CASE("transport.metronome: off is silent; the accent follows the pattern start", "[transport]")
{
    Rig rig(44100.0, 120.0);
    rig.metronome.set(false, 1.0f, false);
    rig.transport.command({TransportCmd::Play, 0});
    rig.run(1000);
    checkClicks(rig, {}, {}, 1.0f);

    Rig shifted(44100.0, 120.0);
    shifted.metronome.set(true, 1.0f, false);
    shifted.metronome.setBarOrigin(1.0); // a pattern that started on beat 2
    shifted.transport.command({TransportCmd::Play, 0});
    shifted.run(6 * 22050 / 64 + 20);
    checkClicks(shifted, {0, 22050, 44100, 66150, 88200, 110250, 132300},
                {false, true, false, false, false, true, false}, 1.0f);
}

TEST_CASE("transport.engine: metronome on + PLAY puts the click on MAIN at the beat frames", "[transport][engine]")
{
    auto e = enginetest::make(false); // no test tone
    const auto& reg = core::builtinParams();
    const auto id = [&](const char* n) { return reg.idOf(n).value_or(core::ParamId {}); };
    REQUIRE(e->bridge().post(engine::ParamChange {id("transport.metronome"), 0, 1.0f, 0}));
    REQUIRE(e->bridge().post(engine::TransportCmd {TransportCmd::Play, 0}));
    const auto out = enginetest::render(*e, 3 * 22050, 256);
    engine::transport::Metronome ref;
    ref.prepare(44100.0);
    const float gain = core::GainLaw::gain(100) * Metronome::kPeak;
    float worst = 0, between = 0;
    for (int k = 0; k < 3; ++k)
        for (int i = 0; i < ref.clickFrames(); ++i)
        {
            const float want = (k == 0 ? ref.accentTable() : ref.beatTable())[i] * gain;
            worst = std::fmax(worst, std::fabs(out.l[static_cast<std::size_t>(k * 22050 + i)] - want));
        }
    for (int i = ref.clickFrames(); i < 22050; ++i)
        between = std::fmax(between, std::fabs(out.l[static_cast<std::size_t>(i)]));
    CHECK(worst < 1e-6f);
    CHECK(between == 0.0f);
    CHECK(out.r == out.l);
    const auto& feed = e->bridge().playhead();
    CHECK(feed.playing);
    CHECK(std::fabs(feed.ppq - 3.0) < 0.01);
}

TEST_CASE("transport.engine: tempo, nudge and pre-count through the bridge; the feed shows them", "[transport][engine]")
{
    auto e = enginetest::make(false);
    const auto& reg = core::builtinParams();
    const auto id = [&](const char* n) { return reg.idOf(n).value_or(core::ParamId {}); };
    REQUIRE(e->bridge().post(engine::ParamChange {id("transport.tempo"), 0, 150.0f, 0}));
    REQUIRE(e->bridge().post(engine::TransportCmd {TransportCmd::Play, 1}));
    enginetest::render(*e, 4096, 256);
    CHECK(e->bridge().playhead().preCount);
    CHECK_FALSE(e->bridge().playhead().playing);
    CHECK(e->bridge().playhead().bpm == 150.0);
    CHECK(e->bridge().playhead().ppq < 0.0);
    // 1 bar at 150 BPM = 1.6 s = 70,560 frames: playing afterwards; the pre-count clicked although the metronome is off.
    auto out = enginetest::render(*e, 70560, 256);
    CHECK(e->bridge().playhead().playing);
    CHECK_FALSE(e->bridge().playhead().preCount);
    CHECK(enginetest::peak(out.l) > 0.1f);
    // Nudge +3 %: the stored tempo (feed) stays, the position runs 3 % faster.
    REQUIRE(e->bridge().post(engine::ParamChange {id("transport.nudge"), 0, 1.0f, 0}));
    enginetest::render(*e, 64, 64);
    const double a = e->bridge().playhead().ppq;
    enginetest::render(*e, 44100, 64);
    const double b = e->bridge().playhead().ppq;
    CHECK(e->bridge().playhead().bpm == 150.0);
    CHECK(std::fabs((b - a) - 2.5 * 1.03) < 1e-3);
    // STOP: back to 0, not playing.
    REQUIRE(e->bridge().post(engine::TransportCmd {TransportCmd::Stop, 1}));
    enginetest::render(*e, 256, 256);
    CHECK_FALSE(e->bridge().playhead().playing);
    CHECK(e->bridge().playhead().ppq == 0.0);
}
