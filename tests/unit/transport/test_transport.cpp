// engine::transport::Transport: drift-free PPQ, tempo at the next block start,
// pause / continue, stop / hard stop, nudge, pre-count; the TransportCmd meanings.
#include "engine/transport/Transport.h"

#include "core/util/musictime/MusicTime.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>

using engine::BlockContext;
using engine::TransportCmd;
using engine::transport::State;
using engine::transport::Transport;

namespace
{
BlockContext block(Transport& t)
{
    BlockContext c {};
    c.sampleRate = t.sampleRate();
    c.frames = engine::kBlockFrames;
    t.advance(c);
    return c;
}
} // namespace

TEST_CASE("transport.drift: 24 h at 300 BPM equal the closed form within 1e-9 quarter", "[transport]")
{
    Transport t;
    t.prepare(44100.0);
    t.setTempo(300.0);
    t.command({TransportCmd::Play, 0});
    const std::int64_t blocks = 24LL * 3600 * 44100 / 64; // 59,535,000 blocks
    BlockContext c {};
    double worst = 0.0, previousEnd = 0.0;
    for (std::int64_t b = 0; b < blocks; ++b)
    {
        t.advance(c);
        if (c.ppqStart != previousEnd) // p0 of every block is exactly p1 of the one before
            FAIL("block " << b << " starts at " << c.ppqStart << ", the previous ended at " << previousEnd);
        if ((b & 0xFFFFF) == 0 || b == blocks - 1)
        {
            const double closed = static_cast<double>((b + 1) * 64) / 8820.0; // frames · 300 / (60 · 44100)
            worst = std::fmax(worst, std::fabs(c.ppqEnd - closed));
        }
        previousEnd = c.ppqEnd;
    }
    CHECK(worst < 1e-9);
    CHECK(std::fabs(t.ppq() - 432000.0) < 1e-9); // 24 h × 300 quarters per minute
}

TEST_CASE("transport.drift: a tempo change every block stays on the closed form (1 h)", "[transport]")
{
    Transport t;
    t.prepare(48000.0);
    t.command({TransportCmd::Play, 0});
    constexpr int kTempos = 7;
    const double tempos[kTempos] = {120.0, 87.3, 300.0, 20.0, 133.7, 174.0, 99.9};
    std::int64_t count[kTempos] = {};
    const std::int64_t blocks = 3600LL * 48000 / 64;
    BlockContext c {};
    double previousEnd = 0.0;
    for (std::int64_t b = 0; b < blocks; ++b)
    {
        const int i = static_cast<int>(b % kTempos);
        t.command({TransportCmd::SetTempo, tempos[i]});
        t.advance(c);
        if (c.ppqStart != previousEnd)
            FAIL("block " << b << " is not contiguous");
        previousEnd = c.ppqEnd;
        ++count[i];
    }
    double closed = 0.0; // Σ blocks at tempo i × 64 × ppqPerFrame(i)
    for (int i = 0; i < kTempos; ++i)
        closed += static_cast<double>(count[i] * 64) * tempos[i] / (60.0 * 48000.0);
    CHECK(std::fabs(c.ppqEnd - closed) < 1e-9);
}

TEST_CASE("transport.tempo: a SetTempo lands on the next block start, the position is kept", "[transport]")
{
    Transport t;
    t.prepare(44100.0);
    t.command({TransportCmd::Play, 0});
    for (int i = 0; i < 100; ++i)
        block(t);
    const double before = t.ppq();
    t.command({TransportCmd::SetTempo, 150.04}); // the 0.1 grid: 150.0
    CHECK(t.bpm() == 150.0);
    const auto c = block(t);
    CHECK(c.ppqStart == before);
    CHECK(c.bpm == 150.0);
    CHECK(std::fabs((c.ppqEnd - c.ppqStart) - 64.0 * 150.0 / (60.0 * 44100.0)) < 1e-15);
    t.command({TransportCmd::SetTempo, 1000.0});
    CHECK(t.bpm() == 300.0);
    t.command({TransportCmd::SetTempo, 1.0});
    CHECK(t.bpm() == 20.0);
}

TEST_CASE("transport.pause: Stop(0) keeps the position, Continue resumes on exactly that PPQ", "[transport]")
{
    Transport t;
    t.prepare(44100.0);
    t.setTempo(133.7);
    t.command({TransportCmd::Play, 0});
    BlockContext last {};
    for (int i = 0; i < 777; ++i)
        last = block(t);
    t.command({TransportCmd::Stop, 0});
    CHECK(t.state() == State::Paused);
    const double paused = t.ppq();
    CHECK(paused == last.ppqEnd);
    auto c = block(t);
    CHECK(c.stoppedThisBlock);
    CHECK_FALSE(c.playing);
    CHECK(c.ppqStart == paused);
    CHECK(c.ppqEnd == paused);
    for (int i = 0; i < 50; ++i)
        block(t);
    CHECK(t.ppq() == paused);
    t.command({TransportCmd::Continue, 0});
    c = block(t);
    CHECK(c.startedThisBlock);
    CHECK(c.playing);
    CHECK(c.ppqStart == paused);
    // Pause is Stop(0).
    t.command({TransportCmd::Pause, 0});
    CHECK(t.state() == State::Paused);
}

TEST_CASE("transport.stop: STOP returns to step 1 with tails; STOP twice raises hardStop once", "[transport]")
{
    Transport t;
    t.prepare(44100.0);
    t.command({TransportCmd::Play, 0});
    for (int i = 0; i < 300; ++i)
        block(t);
    int hard = 0, stops = 0;
    t.command({TransportCmd::Stop, 1}); // first STOP
    auto c = block(t);
    hard += t.hardStopThisBlock();
    stops += c.stoppedThisBlock;
    CHECK(t.state() == State::Stopped);
    CHECK(c.ppqStart == 0.0);
    CHECK(t.locatedThisBlock());
    for (int i = 0; i < 10; ++i)
    {
        c = block(t);
        hard += t.hardStopThisBlock();
        stops += c.stoppedThisBlock;
    }
    t.command({TransportCmd::Stop, 2}); // second STOP (the controller sends Stop(2) when stopped)
    for (int i = 0; i < 10; ++i)
    {
        c = block(t);
        hard += t.hardStopThisBlock();
        stops += c.stoppedThisBlock;
    }
    CHECK(stops == 1);
    CHECK(hard == 1);
    // Stop(1) while stopped is the second STOP too; HardStop = Stop(2) from any state.
    t.command({TransportCmd::Stop, 1});
    block(t);
    CHECK(t.hardStopThisBlock());
    t.command({TransportCmd::Play, 0});
    block(t);
    t.command({TransportCmd::HardStop, 0});
    c = block(t);
    CHECK(t.hardStopThisBlock());
    CHECK(c.stoppedThisBlock);
    CHECK(t.ppq() == 0.0);
    // Stop(1) from pause: back to 0, no hard stop.
    t.command({TransportCmd::Play, 0});
    for (int i = 0; i < 20; ++i)
        block(t);
    t.command({TransportCmd::Stop, 0});
    block(t);
    t.command({TransportCmd::Stop, 1});
    block(t);
    CHECK_FALSE(t.hardStopThisBlock());
    CHECK(t.ppq() == 0.0);
    CHECK(t.state() == State::Stopped);
}

TEST_CASE("transport.play: Play starts at 0, Continue at the located position, Locate jumps", "[transport]")
{
    Transport t;
    t.prepare(44100.0);
    t.command({TransportCmd::Locate, 8.5});
    auto c = block(t);
    CHECK_FALSE(c.playing);
    CHECK(c.ppqStart == 8.5);
    t.command({TransportCmd::Continue, 0}); // ProjectStoreSession: Locate + Continue resumes there
    c = block(t);
    CHECK(c.startedThisBlock);
    CHECK(c.ppqStart == 8.5);
    t.command({TransportCmd::Play, 0}); // already playing: no-op
    c = block(t);
    CHECK_FALSE(c.startedThisBlock);
    CHECK(c.ppqStart > 8.5);
    t.command({TransportCmd::Locate, 2.0}); // while playing: the next block starts there
    c = block(t);
    CHECK(t.locatedThisBlock());
    CHECK(c.ppqStart == 2.0);
    CHECK(c.playing);
    t.command({TransportCmd::Stop, 0});
    block(t);
    t.command({TransportCmd::Play, 0}); // Play from a pause restarts at step 1
    c = block(t);
    CHECK(c.ppqStart == 0.0);
    CHECK(c.startedThisBlock);
    t.command({TransportCmd::Locate, -3.0}); // clamped to 0
    c = block(t);
    CHECK(c.ppqStart == 0.0);
}

TEST_CASE("transport.nudge: +3 % changes ppqPerFrame by exactly 3 % while held, the stored tempo stays", "[transport]")
{
    Transport t;
    t.prepare(44100.0);
    t.setTempo(128.0);
    t.command({TransportCmd::Play, 0});
    block(t);
    const double base = t.ppqPerFrame();
    t.setNudge(+1, 0.03f);
    auto c = block(t);
    const double expected = base * (1.0 + static_cast<double>(0.03f));
    CHECK(std::fabs(t.ppqPerFrame() / base - (1.0 + static_cast<double>(0.03f))) < 1e-15);
    CHECK(std::fabs((c.ppqEnd - c.ppqStart) - 64.0 * expected) < 1e-15);
    CHECK(t.bpm() == 128.0);
    CHECK(std::fabs(c.bpm - 128.0 * (1.0 + static_cast<double>(0.03f))) < 1e-12);
    t.setNudge(-1, 0.03f);
    block(t);
    CHECK(std::fabs(t.ppqPerFrame() / base - (1.0 - static_cast<double>(0.03f))) < 1e-15);
    t.setNudge(0, 0.03f); // release restores the tempo exactly
    c = block(t);
    CHECK(t.ppqPerFrame() == base);
    CHECK(c.bpm == 128.0);
}

TEST_CASE("transport.precount: a 2-bar pre-count at 120 BPM starts playback after exactly 176,400 frames", "[transport]")
{
    Transport t;
    t.prepare(44100.0);
    t.command({TransportCmd::Play, 2});
    CHECK(t.state() == State::PreCount);
    std::int64_t frame = 0, startAt = -1;
    int started = 0;
    for (int b = 0; b < 4000; ++b)
    {
        const auto c = block(t);
        if (t.preCountThisBlock())
        {
            CHECK_FALSE(c.playing);
            CHECK(c.ppqEnd < 0.0);
            CHECK(c.ppqEnd > c.ppqStart); // the clock runs (the metronome clicks)
        }
        if (c.startedThisBlock)
        {
            ++started;
            startAt = frame + t.startFrame();
            CHECK(c.ppqStart < 0.0);
            CHECK(c.ppqEnd > 0.0);
            CHECK(core::musictime::frameOf(0.0, c.ppqStart, t.ppqPerFrame()) == t.startFrame());
        }
        frame += 64;
    }
    CHECK(started == 1);
    CHECK(startAt == 176400);
    CHECK(t.state() == State::Playing);
    // The first pre-count block starts at −8 quarters.
    Transport u;
    u.prepare(44100.0);
    u.command({TransportCmd::Play, 1});
    CHECK(block(u).ppqStart == -4.0);
    // STOP or a pause during the count-in cancels it.
    u.command({TransportCmd::Stop, 0});
    block(u);
    CHECK(u.state() == State::Stopped);
    CHECK(u.ppq() == 0.0);
}

TEST_CASE("transport.precount: 1 bar at 133.7 BPM, 48 kHz starts on the frame of PPQ 0", "[transport]")
{
    Transport t;
    t.prepare(48000.0);
    t.setTempo(133.7);
    t.command({TransportCmd::Play, 1});
    std::int64_t frame = 0, startAt = -1;
    for (int b = 0; b < 2000 && startAt < 0; ++b, frame += 64)
    {
        const auto c = block(t);
        if (c.startedThisBlock)
            startAt = frame + t.startFrame();
    }
    // 4 quarters at 133.7 BPM = 4 · 60 · 48000 / 133.7 = 86163.05… frames → playback on frame 86164.
    CHECK(startAt == static_cast<std::int64_t>(std::ceil(4.0 * 60.0 * 48000.0 / 133.7)));
}

TEST_CASE("transport.sync: the correction hook may change the effective tempo and the phase", "[transport]")
{
    Transport t;
    t.prepare(44100.0);
    struct Hook
    {
        double bpm = 0, phase = 0;
        static void fn(void* u, double, double, double& b, double& p) noexcept
        {
            auto* h = static_cast<Hook*>(u);
            if (h->bpm > 0)
                b = h->bpm;
            p = h->phase;
            h->phase = 0;
        }
    } hook;
    t.setCorrection(&Hook::fn, &hook);
    t.command({TransportCmd::Play, 0});
    block(t);
    hook.bpm = 160.0;
    auto c = block(t);
    CHECK(c.bpm == 160.0);
    CHECK(t.bpm() == 120.0); // the stored tempo is not touched
    hook.phase = 0.5;
    const double before = t.ppq();
    c = block(t);
    CHECK(c.ppqStart == before + 0.5);
    t.setCorrection(nullptr, nullptr);
    c = block(t);
    CHECK(c.bpm == 120.0);
}
