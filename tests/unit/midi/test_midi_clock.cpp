// Clock follower (SYN-012/013, NFR-004 "displayed tempo steady after 2 s"), clock out (SYN-020/021), input timing
// (threading.md: arrival + a fixed latency) and the jitter of the clock-out timestamps (NFR-004: ≤ 0.5 ms) through
// the engine's audio clock (a DLL against the device callback's wake-up jitter), plus the live sender on this host.
#include "tests/unit/midi/MidiTestUtil.h"

#include "engine/midi/ClockFollower.h"
#include "engine/midi/ClockOut.h"
#include "engine/midi/MidiInQueue.h"
#include "engine/midi/MidiOutScheduler.h"
#include "io/midi/MidiSender.h"
#include "io/midi/VirtualPort.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

namespace
{
constexpr double kRate = 44100.0;

double percentile(std::vector<double> v, double p)
{
    if (v.empty())
        return 0.0;
    std::sort(v.begin(), v.end());
    const auto i = static_cast<std::size_t>(std::min<double>(static_cast<double>(v.size() - 1), p * static_cast<double>(v.size())));
    return v[i];
}
} // namespace

TEST_CASE("midi.follower: a 160.0 BPM clock with ±1 ms jitter shows 160.0 steadily after 2 s", "[midi]")
{
    engine::midi::ClockFollower f;
    f.prepare(kRate);
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> jitter(-1.0e-3, 1.0e-3);
    const double period = 60.0 / (160.0 * 24.0); // s
    double maxErr = 0.0;
    bool steady = true;
    std::int64_t nextTick = 0;
    for (std::int64_t block = 0; block * 64 < static_cast<std::int64_t>(40.0 * kRate); ++block)
    {
        const std::int64_t now = block * 64;
        for (;;)
        {
            const double t = (static_cast<double>(nextTick) * period + 0.01 + jitter(rng)) * kRate;
            if (t >= static_cast<double>(now + 64))
                break;
            f.onTick(t);
            ++nextTick;
        }
        f.updateDisplay(now);
        if (static_cast<double>(now) >= 2.0 * kRate)
        {
            steady = steady && f.displayTempo() == 160.0;
            maxErr = std::max(maxErr, std::fabs(f.tempo() - 160.0));
        }
    }
    std::printf("midi.follower: 160.0 BPM +/-1 ms: display steady after 2 s = %d, max |estimate - 160| = %.4f BPM, "
                "reacquisitions = %llu\n",
                steady ? 1 : 0, maxErr, static_cast<unsigned long long>(f.reacquisitions()));
    CHECK(steady);
    CHECK(maxErr < engine::midi::ClockFollower::kHysteresisBpm);
    CHECK(f.reacquisitions() == 0);
}

TEST_CASE("midi.follower: tempo change re-locks, loss after 500 ms (SYN-013)", "[midi]")
{
    engine::midi::ClockFollower f;
    f.prepare(kRate);
    double t = 0.0;
    std::int64_t now = 0;
    auto run = [&](double bpm, double seconds) {
        const double p = 60.0 * kRate / (24.0 * bpm);
        const double end = t + seconds * kRate;
        for (; t < end; t += p)
        {
            f.onTick(t);
            now = static_cast<std::int64_t>(t);
            f.updateDisplay(now);
        }
    };
    run(120.0, 3.0);
    CHECK(std::fabs(f.tempo() - 120.0) < 1e-3);
    CHECK(f.displayTempo() == 120.0);
    run(140.0, 3.0);
    CHECK(std::fabs(f.tempo() - 140.0) < 1e-3);
    CHECK(f.displayTempo() == 140.0);
    CHECK_FALSE(f.lost(now + static_cast<std::int64_t>(0.4 * kRate)));
    CHECK(f.lost(now + static_cast<std::int64_t>(0.6 * kRate)));
}

TEST_CASE("midi.clockout: 24 PPQN ticks on exact frames, transport messages (SYN-020/021)", "[midi]")
{
    engine::midi::ClockOut out;
    out.prepare(kRate);
    engine::midi::MidiOutScheduler sched;
    core::midi::MidiHub hub;
    sched.attach(&hub);
    engine::midi::ClockOutSettings s {true, true, true, false};
    const double bpm = 120.0, ppf = bpm / (60.0 * kRate);
    engine::BlockContext ctx {};
    ctx.sampleRate = kRate;
    ctx.frames = 64;
    ctx.bpm = bpm;
    std::vector<core::midi::MidiOutEvent> got;
    auto drain = [&] {
        core::midi::MidiOutEvent e;
        while (hub.popSync(e) || hub.popOut(e))
            got.push_back(e);
    };
    // 2 bars playing from PPQ 0.
    const int blocks = static_cast<int>(8.0 / (ppf * 64)) + 1;
    for (int b = 0; b < blocks; ++b)
    {
        ctx.ppqStart = b * 64 * ppf;
        ctx.ppqEnd = (b + 1) * 64 * ppf;
        ctx.playing = true;
        ctx.startedThisBlock = b == 0;
        ctx.stoppedThisBlock = false;
        sched.beginBlock(b * 64);
        out.process(ctx, s, 1u, b * 64, ppf, engine::midi::ClockOutEdges {}, 0, sched);
        sched.endBlock();
    }
    drain();
    REQUIRE(! got.empty());
    CHECK(got[0].bytes[0] == 0xFA); // Start before the first tick, same frame
    CHECK(got[0].sampleTime == 0);
    std::vector<std::int64_t> ticks;
    for (const auto& e : got)
        if (e.bytes[0] == 0xF8)
            ticks.push_back(e.sampleTime);
    REQUIRE(ticks.size() >= 8 * 24);
    for (std::size_t n = 0; n < 8 * 24; ++n) // tick n at round(n · 918.75): the stamps carry no jitter
        CHECK(ticks[n] == static_cast<std::int64_t>(std::floor(static_cast<double>(n) * 918.75 + 0.5)));
    // … and their exact time, n · 918.75, to 1/65536 frame (no half-frame rounding left: 11 µs at 44.1 kHz).
    std::size_t k = 0;
    for (const auto& e : got)
        if (e.bytes[0] == 0xF8 && k < 8 * 24)
            CHECK(std::fabs(e.exactTime() - static_cast<double>(k++) * 918.75) <= 1.0 / 65536.0);
    // Stop, then a locate while stopped (SPP), then Continue from there (SPP + Continue).
    got.clear();
    ctx.playing = false;
    ctx.stoppedThisBlock = true;
    sched.beginBlock(blocks * 64);
    out.process(ctx, s, 1u, blocks * 64, ppf, engine::midi::ClockOutEdges {}, 0, sched);
    ctx.stoppedThisBlock = false;
    out.process(ctx, s, 1u, blocks * 64, ppf, engine::midi::ClockOutEdges {true, 0, 0.0}, 32, sched);
    ctx.playing = ctx.startedThisBlock = true;
    ctx.ppqStart = 8.0;
    ctx.ppqEnd = 8.0 + 64 * ppf;
    out.process(ctx, s, 1u, blocks * 64, ppf, engine::midi::ClockOutEdges {}, 32, sched);
    sched.endBlock();
    drain();
    REQUIRE(got.size() >= 5);
    CHECK(got[0].bytes[0] == 0xFC);
    CHECK((got[1].bytes[0] == 0xF2 && got[1].bytes[1] == 32 && got[1].bytes[2] == 0));
    CHECK(got[2].bytes[0] == 0xF2);
    CHECK(got[3].bytes[0] == 0xFB);
    CHECK(got[4].bytes[0] == 0xF8); // PPQ 8.0 is a tick
    // SYN-021: while stopped, the clock runs at the tempo when enabled.
    got.clear();
    s.whileStopped = true;
    ctx.playing = ctx.startedThisBlock = false;
    for (int b = 0; b < 1000; ++b)
    {
        sched.beginBlock(b * 64);
        out.process(ctx, s, 1u, b * 64, ppf, engine::midi::ClockOutEdges {}, 0, sched);
        sched.endBlock();
    }
    drain();
    const double expected = 1000.0 * 64.0 / 918.75;
    CHECK(std::fabs(static_cast<double>(got.size()) - expected) <= 1.0);
}

TEST_CASE("midi.input: events land at arrival + the fixed latency (one block at 64, the period at 256)", "[midi]")
{
    for (const int period : {64, 256, 100})
    {
        engine::midi::AudioClock clock;
        clock.prepare(kRate);
        engine::midi::MidiInScheduler in;
        in.prepare();
        core::midi::MidiHub hub;
        const int latency = engine::midi::MidiInScheduler::latencyFor(period);
        CHECK(latency == (period == 64 ? 64 : (period == 256 ? 256 : 192)));
        // Arrivals spread over many periods, each one before the callback that drains it (device time = frames).
        std::vector<std::int64_t> arrivals, landed;
        std::int64_t frame = 0, blocksDone = 0;
        int k = 0;
        for (int p = 0; p < 200; ++p)
        {
            // Events that arrived during the previous period (between the callbacks).
            for (int j = 0; j < 3; ++j)
            {
                const std::int64_t a = frame - period + (k * 37) % period;
                ++k;
                if (a < 0)
                    continue;
                arrivals.push_back(a);
                hub.pushIn(0, miditest::msg(0x90, 1, 100, miditest::hostNs(static_cast<double>(a), kRate)));
            }
            clock.period(frame, 0, period);
            // The engine renders the blocks this period needs (the adapter: blocks start at multiples of 64).
            while (blocksDone * 64 < frame + period)
            {
                in.collect(hub, 1u, clock, blocksDone * 64, latency);
                for (auto* d = in.begin(); d != in.end(); ++d)
                    landed.push_back(blocksDone * 64 + d->frame);
                ++blocksDone;
            }
            frame += period;
        }
        REQUIRE(landed.size() + 3 >= arrivals.size()); // the last period's events may land after the run
        std::sort(arrivals.begin(), arrivals.end());
        CHECK(in.late() == 0);
        for (std::size_t i = 0; i < landed.size(); ++i)
            REQUIRE(landed[i] == arrivals[i] + latency);
    }
}

TEST_CASE("midi.jitter: clock-out timestamps stay within 0.5 ms under 2 ms callback jitter (NFR-004)", "[midi]")
{
    // The device callback wakes up late by 0…2 ms (uniform) against a DAC clock 60 ppm off nominal. The engine's
    // audio clock (DLL) filters the anchor; the sender's deadline for every clock tick = anchor.timeOf(stamp). The
    // deadline's deviation from the ideal DAC time of that frame is the jitter the sender cannot remove.
    auto e = miditest::make(kRate);
    miditest::Driver d {*e, 256};
    e->midi().hub().setOutputConnected(0, true);
    d.param("midi.out.clock", 0, 1.0f);
    d.param("midi.out.startStop", 0, 1.0f);
    miditest::drainOut(*e);
    e->bridge().post(engine::TransportCmd {engine::TransportCmd::Play, 0.0});
    std::mt19937 rng(11);
    std::uniform_real_distribution<double> late(0.0, 2.0e6);
    const double trueNsPerFrame = 1.0e9 / (kRate * (1.0 + 60e-6));
    const double t0 = 5.0e9;
    std::vector<double> dev, interval, rawInterval;
    std::int64_t frame = static_cast<std::int64_t>(e->blocksRendered()) * 64; // periods of 256 = 4 blocks, aligned
    double sum = 0.0, previous = 0.0, previousRaw = 0.0;
    io::midi::TickSmoother smoother;
    for (int p = 0; p < 3000; ++p) // ≈ 17 s
    {
        const double ideal = t0 + static_cast<double>(frame) * trueNsPerFrame;
        d.runDevicePeriod(static_cast<std::int64_t>(ideal + late(rng)));
        const auto anchor = e->midi().hub().anchor();
        for (const auto& o : miditest::drainOut(*e))
            if (o.bytes[0] == 0xF8 && p > 1000) // after the DLL settled (≈ 6 s)
            {
                const double raw = io::midi::MidiSender::deadlineNs(o, anchor, 0, 0);
                // Raw: the deadline as the anchor gives it; the clock thread smooths it (TickSmoother).
                if (previous > 0.0)
                    rawInterval.push_back(std::fabs(raw - previousRaw - 918.75 * trueNsPerFrame));
                previousRaw = raw;
                const double dl = smoother.deadline(raw, o.exactTime(), anchor.nsPerFrame);
                smoother.commit(raw, o.exactTime(), anchor.nsPerFrame);
                const double x = dl - (t0 + o.exactTime() * trueNsPerFrame);
                dev.push_back(x);
                sum += x;
                // Tick to tick: what a receiver's tempo display reacts to (ideal: 918.75 frames of the DAC clock).
                if (previous > 0.0)
                    interval.push_back(std::fabs(dl - previous - 918.75 * trueNsPerFrame));
                previous = dl;
            }
        frame += 256;
    }
    REQUIRE(dev.size() > 500);
    const double mean = sum / static_cast<double>(dev.size()); // a constant offset (the mean wake-up delay) is latency
    std::vector<double> absdev;
    for (double x : dev)
        absdev.push_back(std::fabs(x - mean));
    const double p99 = percentile(absdev, 0.99) / 1.0e6, mx = *std::max_element(absdev.begin(), absdev.end()) / 1.0e6;
    std::printf("midi.jitter: %zu clock ticks, deadline jitter vs ideal DAC time: p99 = %.3f ms, max = %.3f ms "
                "(callback jitter 0-2 ms, DLL %.2f Hz)\n",
                dev.size(), p99, mx, engine::midi::AudioClock::kBandwidthHz);
    CHECK(p99 <= 0.5);
    // Tick to tick (what a receiver's tempo display reacts to; at 120 BPM a 0.1 BPM step is 17 µs of a 20.8 ms tick):
    // the anchor's per-period corrections show in the raw deadlines; the smoothed ones are exact to a few µs.
    const double rawMax = *std::max_element(rawInterval.begin(), rawInterval.end()) / 1.0e3;
    const double intervalMax = *std::max_element(interval.begin(), interval.end()) / 1.0e3;
    std::printf("midi.jitter: tick-to-tick deviation max = %.2f us raw, %.3f us smoothed\n", rawMax, intervalMax);
    CHECK(intervalMax <= 4.0);
    CHECK(intervalMax * 10.0 < rawMax);
}

TEST_CASE("midi.sender: deadlines on this host (live thread, virtual output)", "[midi]")
{
    // Host only: the sender's own wake-up jitter on this machine (the 0.5 ms acceptance is the device's, measured on
    // hardware); here a loose sanity bound so a loaded build machine does not fail the suite.
    core::midi::MidiHub hub;
    io::midi::VirtualProvider provider(io::midi::VirtualScript {});
    std::string err;
    auto out = provider.openOutput({"virtual:0", "VIRTUAL", true, true}, err);
    REQUIRE(out);
    io::midi::MidiSender sender(hub);
    sender.setOutput(0, out.get());
    hub.setOutputConnected(0, true);
    hub.setOutputLatencyMs(0);
    std::string info;
    REQUIRE(sender.start(info));
    const std::int64_t base = core::midi::monotonicNs() + 20000000; // frame 0 = 20 ms from now
    hub.publishAnchor({0, base, 1.0e9 / kRate, true});
    const int ticks = 400; // 120 BPM: 918.75 frames apart, ≈ 8.3 s
    std::vector<std::int64_t> due;
    for (int n = 0; n < ticks; ++n)
    {
        core::midi::MidiOutEvent e;
        e.port = 0;
        e.size = 1;
        e.bytes[0] = 0xF8;
        e.sampleTime = static_cast<std::int64_t>(std::floor(n * 918.75 + 0.5));
        due.push_back(base + static_cast<std::int64_t>(static_cast<double>(e.sampleTime) * 1.0e9 / kRate));
        while (! hub.pushSync(e)) // the clock thread's queue
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (int i = 0; i < 1000 && sender.stats().sent < static_cast<std::uint64_t>(ticks); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    sender.stop();
    const auto s = sender.stats();
    const auto rec = provider.received();
    REQUIRE(rec.size() == static_cast<std::size_t>(ticks));
    std::vector<double> late;
    for (int n = 0; n < ticks; ++n)
        late.push_back(static_cast<double>(rec[static_cast<std::size_t>(n)].atNs - due[static_cast<std::size_t>(n)]) / 1.0e6);
    const double p99 = percentile(late, 0.99);
    std::printf("midi.sender: %s, %d ticks, lateness p50 = %.3f ms p99 = %.3f ms max = %.3f ms (stats p99 %.0f us, "
                "margin %.0f us)\n",
                info.c_str(), ticks, percentile(late, 0.5), p99, *std::max_element(late.begin(), late.end()), s.p99Us,
                s.marginUs);
    CHECK(*std::min_element(late.begin(), late.end()) >= 0.0); // never early
    CHECK(p99 <= 5.0);
    CHECK(s.clocks == static_cast<std::uint64_t>(ticks));
}

namespace
{
// An output that never takes anything (a USB host that does not read the port).
class StuckOutput final : public io::midi::MidiOutput
{
public:
    io::midi::WriteResult write(const std::uint8_t*, int) override
    {
        tries.fetch_add(1);
        return io::midi::WriteResult::Busy;
    }
    std::atomic<int> tries{0};
};
} // namespace

TEST_CASE("midi.sender: a port that takes nothing holds up neither the clock nor the notes of the others", "[midi]")
{
    core::midi::MidiHub hub;
    io::midi::VirtualProvider provider(io::midi::VirtualScript{});
    std::string err;
    auto out = provider.openOutput({"virtual:0", "VIRTUAL", true, true}, err);
    REQUIRE(out);
    StuckOutput stuck;
    io::midi::MidiSender sender(hub);
    sender.setOutput(0, out.get());
    sender.setOutput(1, &stuck);
    hub.setOutputConnected(0, true);
    hub.setOutputConnected(1, true);
    std::string info;
    REQUIRE(sender.start(info));
    const std::int64_t base = core::midi::monotonicNs() + 20000000;
    hub.publishAnchor({0, base, 1.0e9 / kRate, true});
    const int ticks = 100; // 10 ms apart: 1 s
    for (int n = 0; n < ticks; ++n)
        for (std::uint8_t port : {std::uint8_t{1}, std::uint8_t{0}}) // the stuck port's tick first
        {
            core::midi::MidiOutEvent e;
            e.port = port;
            e.size = 1;
            e.bytes[0] = 0xF8;
            e.sampleTime = n * 441;
            REQUIRE(hub.pushSync(e));
            e.size = 3;
            e.bytes[0] = 0x90;
            e.bytes[1] = 60;
            e.bytes[2] = 100;
            REQUIRE(hub.pushOut(e)); // a note on each port too
        }
    for (int i = 0; i < 1000 && sender.stats().sent < static_cast<std::uint64_t>(2 * ticks); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // past the stale limit of the last tick
    sender.stop();
    const auto s = sender.stats();
    const auto rec = provider.received();
    REQUIRE(rec.size() == static_cast<std::size_t>(2 * ticks));
    std::vector<double> late;
    for (const auto& r : rec)
        if (r.bytes[0] == 0xF8)
            late.push_back(static_cast<double>(r.atNs - base) / 1.0e6 - static_cast<double>(late.size()) * 10.0);
    REQUIRE(late.size() == static_cast<std::size_t>(ticks));
    std::printf("midi.sender: next to a stuck port, %d ticks, lateness p99 = %.3f ms, busy %llu, dropped %llu\n", ticks,
                percentile(late, 0.99), static_cast<unsigned long long>(s.busy),
                static_cast<unsigned long long>(s.dropped));
    CHECK(*std::min_element(late.begin(), late.end()) >= 0.0);
    CHECK(percentile(late, 0.99) <= 5.0);
    CHECK(s.busy > 0);
    CHECK(stuck.tries.load() < 40 * ticks);                     // retried every 1 ms by each thread, not spun on
    CHECK(s.dropped >= static_cast<std::uint64_t>(ticks) - 10); // its stale ticks are dropped, not sent in a burst
}

TEST_CASE("midi.sender: the tick smoother keeps the intervals exact and follows the audio clock without drift",
          "[midi]")
{
    // Raw tick deadlines on an audio clock 100 ppm off the anchor's claimed rate, ±20 µs of per-period anchor noise.
    io::midi::TickSmoother sm;
    const double npf = 1.0e9 / kRate, frames = 918.75, t0 = 1.0e9;
    const auto line = [&](int n) { return t0 + n * frames * npf * (1.0 + 100e-6); };
    double previous = 0.0, worstInterval = 0.0, worstPhase = 0.0;
    for (int n = 0; n < 3000; ++n)
    {
        const double raw = line(n) + (n % 2 != 0 ? 20.0e3 : -20.0e3);
        const double d = sm.deadline(raw, n * frames, npf);
        sm.commit(raw, n * frames, npf);
        if (n > 1500) // settled: on the audio clock's line (no drift) with exact intervals
        {
            worstPhase = std::max(worstPhase, std::fabs(d - line(n)));
            worstInterval = std::max(worstInterval, std::fabs(d - previous - frames * npf * (1.0 + 100e-6)));
        }
        previous = d;
    }
    std::printf("midi.smoother: 100 ppm + 20 us noise: phase within %.2f us, intervals within %.2f us\n",
                worstPhase / 1e3, worstInterval / 1e3);
    CHECK(worstPhase < 2.0e3);
    CHECK(worstInterval < 1.0e3);
    // A 0.5 ms phase step (the anchor moved): followed smoothly — the next tick moves by 1 % of it.
    int n = 3000;
    double raw = line(n) + 0.5e6;
    double d = sm.deadline(raw, n * frames, npf);
    CHECK(std::fabs(d - line(n) - 0.005e6) < 2.0e3);
    sm.commit(raw, n * frames, npf);
    // A 2 ms step (a re-anchor after an xrun, a new port offset): straight to the raw deadline.
    ++n;
    raw = line(n) + 2.5e6;
    CHECK(sm.deadline(raw, n * frames, npf) == raw);
    sm.commit(raw, n * frames, npf);
    // After a Start / Stop (reset): the raw deadline.
    sm.reset();
    ++n;
    CHECK(sm.deadline(line(n) + 0.3e6, n * frames, npf) == line(n) + 0.3e6);
}

TEST_CASE("midi.sender: the wake-up margin follows the measured wake-up latency", "[midi]")
{
    io::midi::DeadlineWaiter w;
    w.learn(0, 0);
    CHECK(w.margin() == io::midi::DeadlineWaiter::kMinMarginNs); // an exact kernel: the least busy wait
    w.learn(800000, 1000000);                                    // woken 0.8 ms late once
    CHECK(w.margin() == 800000 * 3 / 2 + 50000);
    w.learn(0, 1000000000); // 1 s later: the peak has decayed by e^-0.1
    CHECK(w.margin() < 800000 * 3 / 2 + 50000);
    CHECK(w.margin() > 1000000);
    w.learn(0, 60000000000); // a quiet minute: back to the least
    CHECK(w.margin() == io::midi::DeadlineWaiter::kMinMarginNs);
    w.learn(5000000, 1000000); // a 5 ms stall: capped
    CHECK(w.margin() == io::midi::DeadlineWaiter::kMaxMarginNs);
    w.learn(0, 600000000000);
    w.setFloorNs(500000); // midi_spin_us=500
    w.learn(0, 1000000);
    CHECK(w.margin() == 500000);
}
