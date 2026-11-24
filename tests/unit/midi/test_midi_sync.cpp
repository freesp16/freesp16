// External clock sync end to end (SYN-011/012/013): the engine follows a MIDI clock whose ticks arrive the way real
// masters deliver them, and the transport must stay on the master's position for minutes, not only on its tempo.
//
//   jitter models  clean; ±1 ms random; "burst": a DAW that renders its clock with its audio (every tick of a 512-frame
//                  48 kHz block goes out at the block start: ticks 0…10.7 ms late, often two at once), plus a 5 ms
//                  USB-poll quantisation; "spiky": 1 tick in 50 arrives 12 ms late (a scheduler hiccup on the master)
//   start modes    MIDI Start (FA) then ticks; the unit already playing on its own when the sync is switched on
//                  mid-song under a running clock (no Start, no SPP). The unit's own PLAY does not start the transport
//                  under MIDI clock (only the clock's Start / Continue does) — tested on its own below.
//
// The target: the clock's ideal position at every block (tick n at t0 + n·P, delayed by the fixed input latency). A
// tick counted twice moves the transport 1/24 quarter (≈ 20 ms at 120 BPM) for good — the drift this file guards.
#include "tests/unit/midi/MidiTestUtil.h"

#include "engine/midi/MidiInQueue.h"
#include "io/midi/MidiSender.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace
{
constexpr double kRate = 44100.0;

void push(engine::Engine& e, std::uint8_t b0, double frame, int port = 0)
{
    REQUIRE(e.midi().hub().pushIn(port, miditest::msg(b0, 0, 0, miditest::hostNs(frame), port)));
}

enum class Jitter { Clean, Random1ms, Burst, Spiky };
enum class Start { MidiStart, RunningClockEnabledLate };

struct SyncResult
{
    double maxErr = 0;       // samples, after the settling bars
    double endErr = 0;       // samples, mean of the last bar
    double startErr = 0;     // samples, mean of the first measured bar
    double shownBpm = 0;
    std::uint64_t ticksIn = 0;
};

// Arrival frame of tick n (ideal time t), per jitter model.
double arrival(Jitter j, double t, int n, std::mt19937& rng)
{
    switch (j)
    {
        case Jitter::Clean: return t;
        case Jitter::Random1ms: return t + std::uniform_real_distribution<double>(-1.0, 1.0)(rng) * kRate / 1000.0;
        case Jitter::Burst:
        {
            // The master renders 512-frame blocks at 48 kHz and sends each block's ticks at its start (late,
            // quantised), then the USB host polls every 5 ms.
            const double block = 512.0 / 48000.0 * kRate, poll = 0.005 * kRate;
            const double sent = std::ceil(t / block) * block;
            return std::ceil(sent / poll) * poll;
        }
        case Jitter::Spiky: return n % 50 == 49 ? t + 0.012 * kRate : t;
    }
    return t;
}

SyncResult follow(double bpm, Jitter jitter, Start start, int bars)
{
    auto e = miditest::make(kRate);
    miditest::Driver d {*e, 64};
    d.param("transport.tempo", 0, 97.0f); // the project's own tempo, far from the clock's
    if (start != Start::RunningClockEnabledLate)
        d.param("midi.syncSource", 0, 1.0f);
    const double P = 60.0 * kRate / (24.0 * bpm);
    const double t0 = static_cast<double>(d.frame()) + 96.0 * P + 3000.0; // tick 0 of the song
    std::mt19937 rng(5);
    const int total = (bars + 2) * 96;
    std::vector<double> at;
    // A clock that runs before the song starts (two beats... or four bars when the follower is switched on late).
    const int pre = start == Start::RunningClockEnabledLate ? 4 * 96 : 96;
    for (int n = -pre; n < total; ++n)
        at.push_back(arrival(jitter, t0 + n * P, n, rng));
    std::size_t next = 0;
    bool started = false, enabled = start != Start::RunningClockEnabledLate;
    const double L = e->midi().inputLatency();
    SyncResult r;
    double sumS = 0, sumE = 0;
    int nS = 0, nE = 0;
    // Late enable: the unit's PLAY (sync still off) lands somewhere inside a tick; its bar 1 is the clock's tick 0
    // rounded to the tick grid (the follower's sub-tick phase is all it can lock to without a Start or SPP).
    double origin = t0;
    for (;;)
    {
        const double now = static_cast<double>(d.frame());
        if (! started && t0 - P / 2 < now)
        {
            if (start == Start::MidiStart)
                push(*e, 0xFA, t0 - P / 2);
            else
            {
                // Playing on its own clock first (PLAY with the sync off), then the sync is switched on.
                e->bridge().post(engine::TransportCmd {engine::TransportCmd::Play, 0.0});
                d.param("midi.syncSource", 0, 1.0f);
                enabled = true;
            }
            started = true;
        }
        while (next < at.size() && at[next] < now)
            push(*e, 0xF8, at[next++]);
        if (next >= at.size())
            break;
        d.blocks(1);
        if (! started || ! enabled)
            continue;
        const double F = static_cast<double>(d.frame());
        const double ppq = e->bridge().playhead().ppq;
        if (start != Start::MidiStart && nS == 0 && (F - (t0 + L)) / (24.0 * P) >= 8.0)
        {
            // No Start: the unit counts its own bars; its position must hold its offset to the clock's tick grid.
            const double target = (F - (t0 + L)) / (24.0 * P);
            const double offsetTicks = std::nearbyint((ppq - target) * 24.0);
            origin = t0 - offsetTicks * P;
        }
        const double target = (F - (origin + L)) / (24.0 * P);
        const double bar = target / 4.0;
        if (bar < 2.0 || bar >= bars)
            continue;
        const double err = (ppq - target) * 24.0 * P;
        r.maxErr = std::max(r.maxErr, std::fabs(err));
        if (bar < 3.0)
        {
            sumS += err;
            ++nS;
        }
        if (bar >= bars - 1)
        {
            sumE += err;
            ++nE;
        }
    }
    r.startErr = nS > 0 ? sumS / nS : 0;
    r.endErr = nE > 0 ? sumE / nE : 0;
    r.shownBpm = e->bridge().playhead().bpm;
    r.ticksIn = e->midi().stats().ticksIn;
    return r;
}

const char* name(Jitter j)
{
    switch (j)
    {
        case Jitter::Clean: return "clean";
        case Jitter::Random1ms: return "random +/-1 ms";
        case Jitter::Burst: return "DAW burst (512 @ 48k + 5 ms poll)";
        case Jitter::Spiky: return "1 in 50 ticks 12 ms late";
    }
    return "";
}
const char* name(Start s)
{
    switch (s)
    {
        case Start::MidiStart: return "MIDI Start";
        case Start::RunningClockEnabledLate: return "sync enabled mid-song";
    }
    return "";
}
} // namespace

TEST_CASE("midi.sync: no drift over minutes, whatever the master's jitter and however the transport started", "[midi]")
{
    for (const double bpm : {120.0, 160.0, 174.0})
        for (const Jitter j : {Jitter::Clean, Jitter::Random1ms, Jitter::Burst, Jitter::Spiky})
            for (const Start s : {Start::MidiStart, Start::RunningClockEnabledLate})
            {
                const int bars = static_cast<int>(bpm / 4.0 * 2.0); // two minutes
                const auto r = follow(bpm, j, s, bars);
                std::printf("midi.sync: %5.1f BPM, %-34s %-22s bars %3d: start %+8.2f, end %+8.2f, max %7.2f samples, "
                            "shown %.1f\n",
                            bpm, name(j), name(s), bars, r.startErr, r.endErr, r.maxErr, r.shownBpm);
                INFO(bpm << " BPM, " << name(j) << ", " << name(s));
                // No drift: the last bar sits where the first one did (well under a millisecond apart).
                CHECK(std::fabs(r.endErr - r.startErr) <= 22.0);
                // Locked: inside the master's own jitter (the burst model is 0…16 ms late: its mean is latency).
                CHECK(r.maxErr <= (j == Jitter::Burst ? 0.012 * kRate : 0.004 * kRate));
                CHECK(std::fabs(r.shownBpm - bpm) <= 0.1);
            }
}

namespace
{
// An engine following a clean clock on port 0 from `t0` (tick n at t0 + n·P), clock out on output port 0.
struct Slave
{
    explicit Slave(double bpm) : e(miditest::make(kRate)), d(*e, 64), P(60.0 * kRate / (24.0 * bpm))
    {
        d.param("midi.syncSource", 0, 1.0f);
        e->midi().hub().setOutputConnected(0, true);
        d.param("midi.out.clock", 0, 1.0f);
        d.param("midi.out.startStop", 0, 1.0f);
        d.param("midi.out.songPosition", 0, 1.0f);
        t0 = static_cast<double>(d.frame()) + 24.0 * P + 1000.0;
        L = e->midi().inputLatency();
    }
    // Runs blocks until frame `until`, feeding tick times from `ticks` (and the extra messages) as they arrive.
    struct Msg
    {
        double at;
        std::uint8_t b0, b1, b2;
    };
    void run(double until, std::vector<Msg>& q)
    {
        std::sort(q.begin(), q.end(), [](const Msg& a, const Msg& b) { return a.at < b.at; });
        std::size_t i = 0;
        while (static_cast<double>(d.frame()) < until)
        {
            const double now = static_cast<double>(d.frame());
            for (; i < q.size() && q[i].at < now; ++i)
                REQUIRE(e->midi().hub().pushIn(0, miditest::msg(q[i].b0, q[i].b1, q[i].b2, miditest::hostNs(q[i].at))));
            d.blocks(1);
            for (const auto& o : miditest::drainOut(*e))
                out.push_back(o);
            ppq.push_back({static_cast<double>(d.frame()), e->bridge().playhead().ppq});
        }
        q.erase(q.begin(), q.begin() + static_cast<std::ptrdiff_t>(i));
    }
    void ticks(std::vector<Msg>& q, int from, int to) const
    {
        for (int n = from; n < to; ++n)
            q.push_back({t0 + n * P, 0xF8, 0, 0});
    }
    std::unique_ptr<engine::Engine> e;
    miditest::Driver d;
    double P, t0 = 0, L = 0;
    std::vector<core::midi::MidiOutEvent> out;
    std::vector<std::pair<double, double>> ppq; // (frame after the block, playhead ppq)
};
} // namespace

TEST_CASE("midi.sync: MIDI Start lands on its tick's sample, and the clock out follows the clock in tick for tick",
          "[midi]")
{
    for (const double bpm : {98.0, 120.0, 173.3})
    {
        Slave s(bpm);
        std::vector<Slave::Msg> q;
        s.ticks(q, -24, 0); // the master's clock runs before its Start
        q.push_back({s.t0 - s.P / 2, 0xFA, 0, 0});
        s.ticks(q, 0, 8 * 96);
        s.run(s.t0 + 8 * 96 * s.P - s.P, q);
        // The transport's quarter 0 is on the first tick's sample (+ the fixed input latency): the very first block.
        bool first = true;
        double worst = 0.0;
        for (const auto& [frame, p] : s.ppq)
        {
            const double target = (frame - (s.t0 + s.L)) / (24.0 * s.P);
            if (target <= 0.0)
                continue;
            const double err = (p - target) * 24.0 * s.P;
            if (first)
            {
                INFO(bpm << " BPM: first block after Start, error " << err << " samples");
                CHECK(std::fabs(err) < 0.5);
                first = false;
            }
            worst = std::max(worst, std::fabs(err));
        }
        CHECK(worst < 0.5);
        // Clock out (regenerated from the transport): Start on the first tick's frame, then every tick within one
        // frame of the incoming one (+ the input latency) — no drift, no tick lost or added.
        std::vector<std::int64_t> outTicks;
        std::int64_t startAt = -1;
        for (const auto& o : s.out)
        {
            if (o.bytes[0] == 0xFA)
                startAt = o.sampleTime;
            else if (o.bytes[0] == 0xF8 && startAt >= 0)
                outTicks.push_back(o.sampleTime);
        }
        const auto expect = [&](int n) { return s.t0 + s.L + n * s.P; };
        INFO(bpm << " BPM: Start out at " << startAt << ", first tick in at " << expect(0));
        CHECK(std::fabs(static_cast<double>(startAt) - expect(0)) <= 1.0);
        REQUIRE(outTicks.size() >= static_cast<std::size_t>(8 * 96 - 2));
        double maxDev = 0.0;
        for (std::size_t n = 0; n < outTicks.size(); ++n)
            maxDev = std::max(maxDev, std::fabs(static_cast<double>(outTicks[n]) - expect(static_cast<int>(n))));
        std::printf("midi.sync: %.1f BPM slave, %zu clock-out ticks, max |out - in| = %.2f frames\n", bpm,
                    outTicks.size(), maxDev);
        CHECK(maxDev <= 1.0);
    }
}

TEST_CASE("midi.sync: Stop then Continue resumes at the clock's song position; SPP while playing jumps", "[midi]")
{
    Slave s(120.0);
    std::vector<Slave::Msg> q;
    // Start, 2 bars (192 ticks), Stop; the clock keeps running for a bar; Continue; 2 more bars.
    q.push_back({s.t0 - s.P / 2, 0xFA, 0, 0});
    s.ticks(q, 0, 192);
    q.push_back({s.t0 + 191.5 * s.P, 0xFC, 0, 0});
    s.ticks(q, 192, 288);
    q.push_back({s.t0 + 287.5 * s.P, 0xFB, 0, 0});
    s.ticks(q, 288, 480);
    s.run(s.t0 + 470 * s.P, q);
    // After the Continue, tick 288 + k of the stream is song position (192 + k) / 24.
    double worst = 0.0;
    int n = 0;
    for (const auto& [frame, p] : s.ppq)
    {
        const double k = (frame - (s.t0 + s.L)) / s.P - 288.0;
        if (k < 0.0)
            continue;
        worst = std::max(worst, std::fabs(p - (192.0 + k) / 24.0) * 24.0 * s.P);
        ++n;
    }
    INFO("after Continue: max error " << worst << " samples over " << n << " blocks");
    CHECK(n > 100);
    CHECK(worst < 0.5);
    // The clock out sent Stop, then SPP 32 (sixteenths: 8 quarters) and Continue, on the first tick's frame.
    std::vector<std::uint8_t> kinds;
    std::int64_t contAt = -1;
    for (const auto& o : s.out)
        if (o.bytes[0] != 0xF8)
        {
            kinds.push_back(o.bytes[0]);
            if (o.bytes[0] == 0xF2)
                CHECK((o.bytes[1] | (o.bytes[2] << 7)) == 32);
            if (o.bytes[0] == 0xFB)
                contAt = o.sampleTime;
        }
    CHECK(kinds == std::vector<std::uint8_t> {0xFA, 0xFC, 0xF2, 0xFB});
    CHECK(std::fabs(static_cast<double>(contAt) - (s.t0 + s.L + 288 * s.P)) <= 1.0);

    // SPP while the clock runs (a DAW loop back to bar 1): the transport is there by the next tick.
    s.ppq.clear();
    q.push_back({s.t0 + 470.5 * s.P, 0xF2, 0, 0});
    s.ticks(q, 480, 600);
    s.run(s.t0 + 590 * s.P, q);
    worst = 0.0;
    n = 0;
    for (const auto& [frame, p] : s.ppq)
    {
        const double k = (frame - (s.t0 + s.L)) / s.P - 471.0;
        if (k < 1.0)
            continue;
        worst = std::max(worst, std::fabs(p - k / 24.0) * 24.0 * s.P);
        ++n;
    }
    INFO("after SPP 0: max error " << worst << " samples over " << n << " blocks");
    CHECK(n > 100);
    CHECK(worst < 0.5);
}

TEST_CASE("midi.sync: under MIDI clock the unit's PLAY does not start the transport, the clock's Start does", "[midi]")
{
    Slave s(120.0);
    std::vector<Slave::Msg> q;
    s.ticks(q, -48, 96); // the master's clock runs, no Start yet
    s.run(s.t0 + 10 * s.P, q);
    // PLAY (and a device-restore Continue) while following: dropped, the transport stays stopped at 0.
    s.e->bridge().post(engine::TransportCmd {engine::TransportCmd::Play, 0.0});
    s.e->bridge().post(engine::TransportCmd {engine::TransportCmd::Continue, 0.0});
    s.run(s.t0 + 40 * s.P, q);
    CHECK_FALSE(s.e->bridge().playhead().playing);
    CHECK(s.e->bridge().playhead().ppq == 0.0);
    CHECK(s.e->bridge().playhead().externalClock);
    // STOP still works (nothing to stop here, but it is not dropped): the transport stays stopped.
    s.e->bridge().post(engine::TransportCmd {engine::TransportCmd::Stop, 1.0});
    s.run(s.t0 + 44 * s.P, q);
    CHECK_FALSE(s.e->bridge().playhead().playing);
    // The master's Start: playing from its next tick.
    q.push_back({s.t0 + 47.5 * s.P, 0xFA, 0, 0});
    s.ticks(q, 96, 192);
    s.run(s.t0 + 96 * s.P, q);
    CHECK(s.e->bridge().playhead().playing);
    CHECK(s.e->bridge().playhead().ppq > 1.5);
    // Sync off: PLAY is the unit's own again.
    s.e->bridge().post(engine::TransportCmd {engine::TransportCmd::Stop, 1.0});
    s.d.param("midi.syncSource", 0, 0.0f);
    s.d.blocks(4);
    CHECK_FALSE(s.e->bridge().playhead().playing);
    s.e->bridge().post(engine::TransportCmd {engine::TransportCmd::Play, 0.0});
    s.d.blocks(4);
    CHECK(s.e->bridge().playhead().playing);
    CHECK_FALSE(s.e->bridge().playhead().externalClock);
}

TEST_CASE("midi.sync: a clock that drops out and comes back is picked up without a jump back (KEEP TEMPO)", "[midi]")
{
    Slave s(120.0);
    std::vector<Slave::Msg> q;
    q.push_back({s.t0 - s.P / 2, 0xFA, 0, 0});
    s.ticks(q, 0, 192);
    s.ticks(q, 240, 480); // 48 ticks (1 s) missing: the cable was out, the master went on
    s.run(s.t0 + 470 * s.P, q);
    double prev = -1.0, maxStep = 0.0;
    for (const auto& [frame, p] : s.ppq)
    {
        if (prev >= 0.0)
            maxStep = std::max(maxStep, p - prev);
        CHECK(p >= prev); // never backwards
        prev = p;
    }
    // One block is 64 frames ≈ 0.0029 quarter at 120 BPM: no step is more than half a tick beyond that.
    CHECK(maxStep < 64.0 / (24.0 * s.P) + 0.5 / 24.0);
    // Back on the clock's tick grid: the phase error to the nearest tick is a fraction of a sample at the end.
    const auto [frame, p] = s.ppq.back();
    const double ticksIn = (frame - (s.t0 + s.L)) / s.P;
    const double frac = p * 24.0 - ticksIn;
    CHECK(std::fabs(frac - std::nearbyint(frac)) * s.P < 1.0);
}

TEST_CASE("midi.out: every message is due after the engine produced it and when its audio is heard", "[midi]")
{
    // Device periods of 256 frames, a 3-period buffer (768 frames), callbacks late by up to 2 ms: each message's
    // deadline is ≥ the end of the period that produced it (the callback's latest finish), and at the anchor time of
    // its own frame + the output buffer (when that frame leaves the DAC).
    auto e = miditest::make(kRate);
    miditest::Driver d {*e, 256};
    e->midi().hub().setOutputConnected(0, true);
    d.param("midi.out.clock", 0, 1.0f);
    d.param("midi.out.startStop", 0, 1.0f);
    miditest::drainOut(*e);
    e->bridge().post(engine::TransportCmd {engine::TransportCmd::Play, 0.0});
    std::mt19937 rng(9);
    std::uniform_real_distribution<double> late(0.0, 2.0e6);
    const double nsPerFrame = 1.0e9 / kRate, t0 = 3.0e9;
    std::int64_t frame = static_cast<std::int64_t>(e->blocksRendered()) * 64;
    double minSlack = 1e18;
    std::size_t n = 0;
    for (int p = 0; p < 2000; ++p)
    {
        const double callback = t0 + static_cast<double>(frame) * nsPerFrame + late(rng);
        float* out[12] = {d.l.data(), d.r.data()};
        e->process(engine::AudioIO {out, 1, nullptr, 0, 256, static_cast<std::uint64_t>(callback), 768});
        const auto anchor = e->midi().hub().anchor();
        CHECK(anchor.leadFrames == 768.0);
        for (const auto& o : miditest::drainOut(*e))
        {
            const double due = io::midi::MidiSender::deadlineNs(o, anchor, 0, 0);
            minSlack = std::min(minSlack, due - (callback + 256.0 * nsPerFrame));
            ++n;
        }
        frame += 256;
    }
    std::printf("midi.out: %zu messages, min slack after the producing period's end = %.2f ms\n", n, minSlack / 1e6);
    CHECK(n > 500);
    CHECK(minSlack > 0.0);

    // Unknown output latency: two periods; never less than one period.
    engine::midi::AudioClock c;
    c.prepare(kRate);
    c.period(0, 1000, 256, 0);
    CHECK(c.anchor().leadFrames == 512.0);
    c.period(256, 1000 + 5804988, 256, 100);
    CHECK(c.anchor().leadFrames == 256.0);
    c.period(512, 1000 + 2 * 5804988, 256, 1024);
    CHECK(c.anchor().leadFrames == 1024.0);
}

TEST_CASE("midi.clockout: a tick on a block boundary goes out once (playing and free-running)", "[midi]")
{
    // 120 BPM at 44.1 kHz: a tick every 918.75 frames, so every 256th tick sits exactly on a block boundary; other
    // tempos put ticks within rounding of one. Every tick n/24 up to the end must go out once, in order.
    for (const double bpm : {120.0, 98.0, 137.8125, 173.3})
    {
        engine::midi::ClockOut out;
        out.prepare(kRate);
        engine::midi::MidiOutScheduler sched;
        core::midi::MidiHub hub;
        sched.attach(&hub);
        const engine::midi::ClockOutSettings s {true, false, false, true};
        const double ppf = bpm / (60.0 * kRate);
        engine::BlockContext ctx {};
        ctx.bpm = bpm;
        std::vector<std::int64_t> ticks;
        const int blocks = 20000; // ≈ 29 s
        for (const bool playing : {true, false})
        {
            ticks.clear();
            for (int b = 0; b < blocks; ++b)
            {
                ctx.ppqStart = b * 64 * ppf; // as the transport computes it: anchor + frames · rate
                ctx.ppqEnd = (b + 1) * 64 * ppf;
                ctx.playing = playing;
                ctx.startedThisBlock = playing && b == 0;
                sched.beginBlock(b * 64);
                out.process(ctx, s, 1u, b * 64, ppf, engine::midi::ClockOutEdges {}, 0, sched);
                sched.endBlock();
                core::midi::MidiOutEvent e;
                while (hub.popOut(e))
                    ticks.push_back(e.sampleTime);
            }
            const double expected = std::ceil(blocks * 64 * ppf * 24.0);
            INFO(bpm << " BPM, " << (playing ? "playing" : "stopped"));
            CHECK(std::fabs(static_cast<double>(ticks.size()) - expected) <= (playing ? 0.0 : 1.0));
            bool increasing = true;
            for (std::size_t i = 1; i < ticks.size(); ++i)
                increasing = increasing && ticks[i] > ticks[i - 1];
            CHECK(increasing);
        }
    }
}

TEST_CASE("midi.out: with the default preferences a message is due exactly when its frame is heard", "[midi]")
{
    // OUTPUT LATENCY is an extra trim (default 0): the anchor's lead (the output buffer) is the whole compensation.
    const auto* latency = core::builtinParams().find("midi.outLatency");
    const auto* offset = core::builtinParams().find("midi.portOffset");
    REQUIRE(latency != nullptr);
    REQUIRE(offset != nullptr);
    CHECK(latency->defaultValue == 0.0f);
    auto e = miditest::make(kRate);
    CHECK(e->midi().hub().outputLatencyMs() == 0); // the hub before the preferences are posted
    miditest::Driver d {*e, 256};
    d.param("midi.outLatency", 0, latency->defaultValue);
    d.param("midi.portOffset", 0, offset->defaultValue);
    e->midi().hub().setOutputConnected(0, true);
    d.param("midi.out.clock", 0, 1.0f);
    e->bridge().post(engine::TransportCmd {engine::TransportCmd::Play, 0.0});
    std::size_t n = 0;
    for (int p = 0; p < 400; ++p) // 2.3 s
    {
        float* out[12] = {d.l.data(), d.r.data()};
        const auto frame = static_cast<std::int64_t>(e->blocksRendered()) * 64;
        e->process(engine::AudioIO {out, 1, nullptr, 0, 256,
                                    static_cast<std::uint64_t>(3.0e9 + static_cast<double>(frame) * 1.0e9 / kRate),
                                    768});
        const auto anchor = e->midi().hub().anchor();
        for (const auto& o : miditest::drainOut(*e))
        {
            const double due = io::midi::MidiSender::deadlineNs(o, anchor, e->midi().hub().outputLatencyMs(),
                                                                e->midi().hub().portOffsetMs(0));
            CHECK(due == anchor.timeOf(static_cast<double>(o.sampleTime) + anchor.leadFrames));
            ++n;
        }
    }
    CHECK(n > 80);
}

TEST_CASE("midi.sync: a MIDI Start while playing restarts like a stop and a start (Stop then Start out)", "[midi]")
{
    Slave s(120.0);
    std::vector<Slave::Msg> q;
    q.push_back({s.t0 - s.P / 2, 0xFA, 0, 0});
    s.ticks(q, 0, 192);
    // The master restarts (Start while playing, the next tick is beat 0 again) right before its tick: both in one
    // block, so the transport stops and starts between two blocks (a restart, no stopped block in between).
    q.push_back({s.t0 + 192 * s.P - 2.0, 0xFA, 0, 0});
    s.ticks(q, 192, 300);
    s.run(s.t0 + 290 * s.P, q);
    std::vector<std::uint8_t> kinds;
    std::vector<std::int64_t> at;
    for (const auto& o : s.out)
        if (o.bytes[0] != 0xF8)
        {
            kinds.push_back(o.bytes[0]);
            at.push_back(o.sampleTime);
        }
    // Before: no edge at all (the transport was playing at both block ends): neither Stop nor Start went out.
    REQUIRE(kinds == std::vector<std::uint8_t> {0xFA, 0xFC, 0xFA});
    CHECK(at[1] <= at[2]);
    CHECK(at[2] - at[1] < 64); // the same block
    CHECK(std::fabs(static_cast<double>(at[2]) - (s.t0 + s.L + 192 * s.P)) <= 1.0);
    // The transport is at beat 0 on tick 192.
    double worst = 0.0;
    for (const auto& [frame, p] : s.ppq)
    {
        const double k = (frame - (s.t0 + s.L)) / s.P - 192.0;
        if (k > 0.0)
            worst = std::max(worst, std::fabs(p - k / 24.0) * 24.0 * s.P);
    }
    CHECK(worst < 0.5);
}
