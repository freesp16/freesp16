// The MIDI output threads (spec 02-features/midi.md MID-002/070, sync-and-transport.md SYN-020, NFR-004).
//
// Two threads, both SCHED_FIFO when the system allows it (best effort):
//
//   clock thread   (priority 80, above the audio thread and the MIDI input threads) the hub's sync queue only: Timing
//                  Clock, Start / Continue / Stop and Song Position, which the engine stamps with the exact
//                  (sub-sample) frame they belong to. Nothing else ever runs on it, so a tick is never held up by
//                  notes, forwarding, lamp feedback or a port that does not take data: it sleeps to each tick's
//                  absolute deadline and writes it.
//   sender thread  (priority 65, below the audio thread) the hub's out queue (notes, CCs, program changes …), the
//   forwarder (MID-002: copies
//                  the inputs' messages to OUT + MERGE A/B and THRU A/B ports at once, A = input port 0, B = input port
//                  1, except to a port in hardware THRU) and sendNow (controller lamps).
//
// Deadline of a message = the audio clock anchor (the engine's DLL-filtered period start: the deadlines follow the
// audio clock, so the MIDI clock neither drifts from the audio nor carries the audio callback's wake-up jitter) + the
// anchor's lead (when the audio of that frame is heard: the device's output buffer) + the output latency
// (midi.outLatency, extra delay for slow receiving gear) + the port's offset (midi.portOffset, −50…+50 ms). A clock
// tick's deadline is then smoothed against the previous tick's (TickSmoother): exact intervals, no wobble.
//
// The wait (DeadlineWaiter): clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME) to a margin before the deadline, then a
// busy wait to the deadline itself. The margin follows this thread's own measured wake-up latency (how late the
// kernel wakes it: its recent peak × 1.5 + 50 µs, the peak decaying over ~10 s, 100 … 500 µs), so a kernel that wakes
// late now and then (no PREEMPT_RT: USB, SD card or display interrupts) still gives exact ticks. The busy wait is
// bounded (≤ 0.5 ms per message): a real-time thread that spins starves the kernel's per-CPU workers (kworker,
// ksoftirqd) on its core, and the drivers that move the MIDI bytes run there. Both threads poll their queues at least
// every 1 ms.
//
// Ports: a write never blocks (MidiOutput::write); a port that is busy (a USB host that does not read) keeps its
// message and is retried 1 ms later — sleeping, never spinning — while the other ports go on; on the clock thread a
// message that is still busy 50 ms past its deadline is dropped (a burst of stale ticks would be worse than none). Each
// port has a plain mutex (the two threads may both write to it, never at once, and the message thread swaps it on
// hot-plug under that lock; held only for one non-blocking write). Not a priority-inheritance mutex: the unit's
// kernel (3.0, ARM) may lack PI futexes, and then glibc refuses to create one.
//
// Jitter (NFR-004: ≤ 0.5 ms p99): every write's lateness (write time − deadline) goes into a 10 µs histogram, one for
// the clock thread and one for the sender; stats() reports p50 / p99 / max of the clock's (what a receiver's tempo
// display shows) and p99 of the sender's. A message already past its deadline by > 1 ms when written (a negative
// offset larger than the latency, a stalled thread) is counted late.
#pragma once

#include "core/util/midi/MidiWire.h"
#include "io/midi/MidiPort.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace io::midi
{
struct SenderStats
{
    std::uint64_t sent = 0, late = 0, forwarded = 0, clocks = 0, dropped = 0, samples = 0, busy = 0;
    double p50Us = 0, p99Us = 0, maxUs = 0; // the clock thread's writes (ticks, transport) against their deadlines
    double notesP99Us = 0;                  // the sender thread's
    double marginUs = 0;                    // the clock thread's wake-up margin now (DeadlineWaiter)
};

// Lateness histogram: 10 µs bins, 0 … 20 ms. One writer thread; any thread reads (racily, for a report).
class LatenessHistogram
{
public:
    static constexpr int kBins = 2000;
    void record(double lateNs) noexcept;
    std::uint64_t samples() const noexcept { return samples_.load(std::memory_order_relaxed); }
    double maxUs() const noexcept { return maxNs_.load(std::memory_order_relaxed) / 1000.0; }
    // The q-quantile (0…1) in µs (the bin's upper edge); 0 without samples.
    double percentileUs(double q) const noexcept;
    void reset() noexcept;

private:
    std::array<std::atomic<std::uint32_t>, kBins + 1> bins_{};
    std::atomic<std::uint64_t> samples_{0};
    std::atomic<double> maxNs_{0.0};
};

// Sleeps to absolute CLOCK_MONOTONIC deadlines with a margin learnt from this thread's wake-up latency, then busy-waits
// to the deadline (see the top of this file). One thread.
class DeadlineWaiter
{
public:
    static constexpr std::int64_t kMinMarginNs = 100000;  // 100 µs
    static constexpr std::int64_t kMaxMarginNs = 500000;  // 500 µs: the most a message may spin
    static constexpr double kDecaySeconds = 10.0;         // the wake-up latency peak's memory

    // The time to wake up at for a write at `deadline`.
    std::int64_t wakeFor(std::int64_t deadline) const noexcept { return deadline - margin(); }
    std::int64_t margin() const noexcept { return margin_.load(std::memory_order_relaxed); }
    // clock_nanosleep to absolute `t`; how late the kernel woke the thread feeds the margin. `now` = the time before.
    void sleepUntil(std::int64_t t, std::int64_t now) noexcept;
    // Busy-waits to `deadline`.
    static void spinUntil(std::int64_t deadline) noexcept;
    // The margin's model, without a clock (tests): a wake-up `latencyNs` late, `sinceNs` after the previous one.
    void learn(std::int64_t latencyNs, std::int64_t sinceNs) noexcept;
    // The least margin (setting midi_spin_us; default kMinMarginNs). Any thread.
    void setFloorNs(std::int64_t ns) noexcept;

private:
    double peakNs_ = 0.0;
    std::int64_t lastWake_ = 0;
    std::atomic<std::int64_t> floor_{kMinMarginNs};
    std::atomic<std::int64_t> margin_{kMinMarginNs * 3};
};

// The clock ticks' deadlines on one port, smoothed (the clock thread). The raw deadline of a tick comes from the audio
// clock anchor, which the engine's DLL moves a little every audio period (it follows the callback's wake-up times: a
// phase step of a few µs … tens of µs and a rate that wanders by up to a few hundred ppm when the callbacks are late
// by milliseconds): harmless for a note, but a receiver measuring tick-to-tick intervals shows it as tempo wobble.
// So each tick is due where the previous one was plus the exact frames between them (the stamps carry no jitter) at
// this smoother's own rate, and only a small part of its distance from the raw deadline corrects that time (kGain)
// and the rate (kRateGain): a second-order loop (critically damped, ~100 ticks: ~2 s at 120 BPM) that follows the
// audio clock — phase and rate, so the ticks never drift from the audio — while the intervals stay exact to ~1 µs
// (tests: 2.5 µs at most with every audio callback up to 2 ms late, against 43 µs for the raw deadlines).
// An error over 1 ms (a re-anchor after an xrun, a changed port offset) or a pause jumps straight to the raw deadline;
// so does the first tick after a Start / Continue / Stop / Song Position.
class TickSmoother
{
public:
    static constexpr double kGain = 0.01;
    static constexpr double kRateGain = kGain * kGain / 2.0;
    static constexpr double kJumpNs = 1.0e6;
    static constexpr double kGapNs = 2.0e9;

    // The deadline of the tick at engine frame `frame` (fractional) whose raw deadline is `raw`; `nsPerFrame` = the
    // anchor's rate (where the smoother starts from, and its bounds: ±1000 ppm).
    double deadline(double raw, double frame, double nsPerFrame) const noexcept
    {
        return plan(raw, frame, nsPerFrame).time;
    }
    // That tick went out (or was dropped): the next one is due from here.
    void commit(double raw, double frame, double nsPerFrame) noexcept
    {
        const Plan p = plan(raw, frame, nsPerFrame);
        time_ = p.time;
        rate_ = p.rate;
        frame_ = frame;
        valid_ = true;
    }
    void reset() noexcept { valid_ = false; }

private:
    struct Plan
    {
        double time, rate;
    };
    Plan plan(double raw, double frame, double nsPerFrame) const noexcept
    {
        const double frames = frame - frame_;
        if (! valid_ || frames <= 0.0 || frames * nsPerFrame > kGapNs)
            return {raw, nsPerFrame};
        const double predicted = time_ + frames * rate_, error = raw - predicted;
        if (error > kJumpNs || error < -kJumpNs)
            return {raw, nsPerFrame};
        double rate = rate_ + kRateGain * error / frames;
        const double lo = nsPerFrame * 0.999, hi = nsPerFrame * 1.001;
        rate = rate < lo ? lo : (rate > hi ? hi : rate);
        return {predicted + kGain * error, rate};
    }
    double time_ = 0.0, frame_ = 0.0, rate_ = 0.0;
    bool valid_ = false;
};

class MidiSender
{
public:
    static constexpr int kPorts = core::midi::kMaxPorts;
    static constexpr int kRing = 1024;                 // per output port and thread
    static constexpr std::int64_t kRetryNs = 1000000;  // a busy port is tried again after 1 ms
    static constexpr std::int64_t kStaleNs = 50000000; // the clock thread drops what is still busy 50 ms late
    static constexpr int kClockPriority = 80, kSenderPriority = 65;

    explicit MidiSender(core::midi::MidiHub& hub) : hub_(hub) {}
    ~MidiSender() { stop(); }
    MidiSender(const MidiSender&) = delete;
    MidiSender& operator= (const MidiSender&) = delete;

    // Message thread: the output port of slot `port` (null = none). The pointer stays owned by the caller, who must
    // clear it here before destroying the port (this waits for a write in progress on it).
    void setOutput(int port, MidiOutput* output);
    bool start(std::string& info);
    void stop();
    bool running() const noexcept { return sender_.joinable(); }
    // The least busy wait before a deadline, µs (setting midi_spin_us): the margin never goes below it.
    void setSpinUs(int us) noexcept
    {
        clockWait_.setFloorNs(us < 0 ? 0 : us * 1000LL);
        noteWait_.setFloorNs(us < 0 ? 0 : us * 1000LL);
    }
    // A port that failed a write (gone): cleared by the message thread's hot-plug pass.
    bool failed(int port) const noexcept { return (failed_.load() >> port & 1u) != 0; }
    void clearFailed(int port) noexcept { failed_.fetch_and(~(1u << port)); }
    // A port whose socket passes its input on in hardware (MidiService: the unit's DIN THRU): nothing is forwarded to
    // it. Any thread.
    void setHardwareThru(int port, bool on) noexcept
    {
        if (port >= 0 && port < kPorts)
            on ? hardwareThru_.fetch_or(1u << port) : hardwareThru_.fetch_and(~(1u << port));
    }

    SenderStats stats() const;
    void resetStats();

    // A message to write as soon as the sender wakes (≤ 1 ms), not stamped by the engine — controller lamp
    // feedback (UI-135). One producer thread (the message thread). False when the queue is full. Not counted in
    // stats() (sentNow() counts them).
    bool sendNow(int port, const std::uint8_t* bytes, int size) noexcept;
    std::uint64_t sentNow() const noexcept { return sentNow_.load(std::memory_order_relaxed); }

    // The deadline of an out event under `anchor` (ns; also used by tests).
    static double deadlineNs(const core::midi::MidiOutEvent& e, const core::midi::ClockAnchor& a, int latencyMs,
                             int offsetMs) noexcept
    {
        return a.timeOf(e.exactTime() + a.leadFrames) + (latencyMs + offsetMs) * 1.0e6;
    }

private:
    // One thread's per-port queues of stamped messages (that thread only).
    struct Lanes
    {
        std::array<std::array<core::midi::MidiOutEvent, kRing>, kPorts> ring{};
        std::array<int, kPorts> head{}, count{};
        std::array<std::int64_t, kPorts> retryAt{}; // a busy port's next try
        std::array<TickSmoother, kPorts> ticks{};   // the clock thread's
        bool push(const core::midi::MidiOutEvent& e) noexcept;
        const core::midi::MidiOutEvent& front(int p) const noexcept
        {
            return ring[static_cast<std::size_t>(p)][static_cast<std::size_t>(head[static_cast<std::size_t>(p)])];
        }
        void pop(int p) noexcept;
    };
    enum class Lane : std::uint8_t
    {
        Clock,
        Notes
    };

    void runClock();
    void runSender();
    // Waits for and writes the earliest due head of `lanes` (or sleeps ≤ 1 ms when none is due that soon).
    void serve(Lane lane, Lanes& lanes, DeadlineWaiter& waiter);
    void forward() noexcept;
    // The written / dropped head of a clock lane (its raw deadline and the anchor's rate: the tick smoother's input).
    void popClock(Lanes& lanes, int port, double raw, double nsPerFrame) noexcept;
    WriteResult write(int port, const std::uint8_t* bytes, int size) noexcept;
    bool startThread(std::thread& t, int priority, void (MidiSender::*body)());

    core::midi::MidiHub& hub_;
    std::thread clock_, sender_;
    std::atomic<bool> quit_{false};
    std::atomic<std::uint32_t> failed_ {0}, hardwareThru_ {0};
    std::array<std::mutex, kPorts> portLocks_; // one port's writes ↔ its swap (message thread)
    std::array<MidiOutput*, kPorts> outputs_ {};
    Lanes clockLanes_, noteLanes_;
    DeadlineWaiter clockWait_, noteWait_;
    // Stats (the threads write, any thread reads; relaxed counters).
    LatenessHistogram clockLate_, noteLate_;
    std::atomic<std::uint64_t> sent_{0}, late_{0}, forwarded_{0}, clocks_{0}, dropped_{0}, busy_{0};
    core::SpscQueue<core::midi::MidiOutEvent, 512> now_ {}; // sendNow (message thread → sender)
    std::atomic<std::uint64_t> sentNow_ {0};
    std::string info_;
};
} // namespace io::midi
