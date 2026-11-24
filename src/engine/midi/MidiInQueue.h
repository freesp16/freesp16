// MIDI input timing (spec 01-architecture/threading.md "Timing": events are timestamped on arrival and scheduled at
// arrival + a fixed latency, so timing is jitter-free, not just fast).
//
//   port thread:  parse + CLOCK_MONOTONIC stamp → core::midi::MidiHub::pushIn (one SPSC per port, core/util/midi)
//   audio thread: AudioClock maps host time ↔ engine sample time with the device callback's anchor (filtered by a
//                 second-order DLL, so the callback's wake-up jitter does not reach the timestamps);
//                 MidiInScheduler drains every port, schedules each event at frameOf(arrival) + inputLatency() and
//                 hands out the events due in the current 64-frame block with their in-block frame.
//
// Fixed latency (our choice): one block when the device period is one block (the spec's 1.45 ms at 44.1 kHz); with a
// longer period P every event of the period before the callback must still land in the future, so the latency is P
// rounded up to the block (plus one block when P is not a multiple of 64: the adapter's carried-over frames). 256-frame
// periods: 256 frames = 5.8 ms. An event that would land before the block (a stalled callback) plays at frame 0 and is
// counted late().
#pragma once

#include "core/util/midi/MidiWire.h"
#include "engine/EngineApi.h"

#include <array>
#include <cstdint>

namespace engine::midi
{
using MidiEvent = core::midi::MidiEvent;       // engine::midi::MidiEvent
using MidiOutEvent = core::midi::MidiOutEvent;

// Host time ↔ engine sample time (audio thread; the sender reads the published anchor).
class AudioClock
{
public:
    static constexpr double kBandwidthHz = 0.25; // DLL bandwidth: callback jitter filtered, ppm drift followed
    static constexpr std::int64_t kResyncNs = 20000000; // a 20 ms error (xrun, device restart) re-anchors

    void prepare(double sampleRate) noexcept;
    // Once per device period: engine frame of the period's first output frame, the callback's monotonic time (0 = no
    // clock: tests and offline renders, then host time is synthetic = frame · nsPerFrame), frames in the period, frames
    // until that first frame is heard (the device's output buffer; 0 = unknown: two periods, the usual buffer). The
    // anchor's lead is that latency, never under one period (a message is then never due before it is produced).
    void period(std::int64_t frame, std::int64_t hostNs, int frames, int outputLatencyFrames = 0) noexcept;
    core::midi::ClockAnchor anchor() const noexcept { return anchor_; }
    double frameOf(std::int64_t hostNs) const noexcept
    {
        return static_cast<double>(anchor_.frame) +
               static_cast<double>(hostNs - anchor_.hostNs) / anchor_.nsPerFrame;
    }
    int periodFrames() const noexcept { return periodFrames_; }
    bool synthetic() const noexcept { return synthetic_; }

private:
    double sampleRate_ = 44100.0;
    double nominalNsPerFrame_ = 1.0e9 / 44100.0;
    core::midi::ClockAnchor anchor_ {};
    double predictedNs_ = 0.0;      // DLL: filtered time of anchor_.frame
    bool init_ = false, synthetic_ = true;
    int periodFrames_ = kBlockFrames;
};

// A due event: the message and its frame inside the current block.
struct DueEvent
{
    MidiEvent event;
    double exactFrame; // scheduled engine frame, fractional, not clamped when late (the clock follower's tick time)
    int frame;         // 0…63 inside the block
};

class MidiInScheduler
{
public:
    static constexpr int kMaxPending = 512;  // events scheduled for later blocks (≥ 2 periods of a busy stream)
    static constexpr int kMaxDue = 256;      // events handed out per block; the rest waits for the next block

    void prepare() noexcept { numPending_ = numDue_ = 0; }
    static int latencyFor(int periodFrames) noexcept
    {
        const int p = periodFrames < kBlockFrames ? kBlockFrames : periodFrames;
        const int up = (p + kBlockFrames - 1) / kBlockFrames * kBlockFrames;
        return p % kBlockFrames == 0 ? up : up + kBlockFrames;
    }
    // Step 2: drain the ports' queues (mask = ports to read), schedule, collect the events of block
    // [blockStart, blockStart + 64) in time order.
    void collect(core::midi::MidiHub& hub, std::uint32_t portMask, const AudioClock& clock, std::int64_t blockStart,
                 int latency) noexcept;
    const DueEvent* begin() const noexcept { return due_.data(); }
    const DueEvent* end() const noexcept { return due_.data() + numDue_; }
    int count() const noexcept { return numDue_; }
    std::uint64_t late() const noexcept { return late_; }
    std::uint64_t dropped() const noexcept { return dropped_; }
    std::uint64_t received() const noexcept { return received_; }

private:
    struct Pending
    {
        MidiEvent event;
        double at;
        std::uint32_t seq;
    };
    void schedule(const MidiEvent& e, double at) noexcept;

    std::array<Pending, kMaxPending> pending_; // written before read (no braces: GCC 11 build time)
    std::array<DueEvent, kMaxDue> due_;
    int numPending_ = 0, numDue_ = 0;
    std::uint32_t seq_ = 0;
    std::uint64_t late_ = 0, dropped_ = 0, received_ = 0;
};
} // namespace engine::midi
