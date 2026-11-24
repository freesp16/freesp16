// The MIDI sender thread (spec 02-features/midi.md MID-002/070, NFR-004).
//
// Pops the engine's sample-stamped messages from the hub (core/util/midi/MidiWire.h), converts each stamp to a
// monotonic deadline — the audio clock anchor (the engine's DLL-filtered period start) + the output latency
// (midi.outLatency) + the port's offset (midi.portOffset, −50…+50 ms) — and writes it at that deadline:
// clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME) to `spinUs` before it, then a short busy wait (the sleep's
// wake-up latency is the jitter; the spin trades ≤ 1 % of a core for it). It polls the hub at least every 1 ms.
// The forwarder (MID-002) copies the inputs' messages to OUT + MERGE A/B and THRU A/B ports at once (A = input port
// 0, B = input port 1), except to a port in hardware THRU (setHardwareThru). Role::MidiOut (core/util/ThreadRole.h):
// SCHED_FIFO below the audio thread when the system allows it (best effort).
//
// Jitter (NFR-004: ≤ 0.5 ms p99): every write's lateness (write time − deadline) goes into a 10 µs histogram; stats()
// reports p50 / p99 / max. Deadlines already past by > 1 ms when popped (a negative offset larger than the latency,
// a stalled sender) are written at once and counted late.
#pragma once

#include "core/util/PiMutex.h"
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
    std::uint64_t sent = 0, late = 0, forwarded = 0, clocks = 0, dropped = 0, samples = 0;
    double p50Us = 0, p99Us = 0, maxUs = 0;
};

class MidiSender
{
public:
    static constexpr int kPorts = core::midi::kMaxPorts;
    static constexpr int kRing = 1024; // per output port
    static constexpr int kBins = 2000; // 10 µs bins: 0 … 20 ms

    explicit MidiSender(core::midi::MidiHub& hub) : hub_(hub) {}
    ~MidiSender() { stop(); }
    MidiSender(const MidiSender&) = delete;
    MidiSender& operator= (const MidiSender&) = delete;

    // Message thread: the output port of slot `port` (null = none). The pointer stays owned by the caller, who must
    // clear it here before destroying the port.
    void setOutput(int port, MidiOutput* output);
    bool start(std::string& info); // the thread takes Role::MidiOut (core/util/ThreadRole.h)
    void stop();
    bool running() const noexcept { return thread_.joinable(); }
    void setSpinUs(int us) noexcept { spinUs_.store(us < 0 ? 0 : us); }
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
        return a.timeOf(static_cast<double>(e.sampleTime)) + (latencyMs + offsetMs) * 1.0e6;
    }

private:
    void run();
    void forward() noexcept;
    bool write(int port, const std::uint8_t* bytes, int size) noexcept;
    void record(double latenessNs) noexcept;

    core::midi::MidiHub& hub_;
    std::thread thread_;
    std::atomic<bool> quit_ {false};
    std::atomic<int> spinUs_ {100};
    std::atomic<std::uint32_t> failed_ {0}, hardwareThru_ {0};
    core::PiMutex portsMutex_; // message thread ↔ sender only (never the audio thread); priority inheritance
    std::array<MidiOutput*, kPorts> outputs_ {};
    // Sender-thread state.
    std::array<std::array<core::midi::MidiOutEvent, kRing>, kPorts> ring_ {};
    std::array<int, kPorts> head_ {}, count_ {};
    // Stats (sender writes, any thread reads; relaxed counters, histogram read racily for a report).
    std::array<std::atomic<std::uint32_t>, kBins + 1> hist_ {};
    std::atomic<std::uint64_t> sent_ {0}, late_ {0}, forwarded_ {0}, clocks_ {0}, dropped_ {0}, samples_ {0};
    std::atomic<double> maxNs_ {0.0};
    core::SpscQueue<core::midi::MidiOutEvent, 512> now_ {}; // sendNow (message thread → sender)
    std::atomic<std::uint64_t> sentNow_ {0};
};
} // namespace io::midi
