// Sample-stamped MIDI output (spec 02-features/midi.md MID-047, MID-070).
// Audio thread only, no allocation. Every message carries the engine sample time it belongs to; endBlock() hands the
// block's messages, in time order, to the hub's out SPSC, where io::midi::MidiSender converts sample time to
// monotonic time (anchor + output latency + per-port offset) and sleeps to each absolute deadline.
//
// Notes: note() sends Note On now and keeps the Note Off for sampleTime + length; a 16 × 128-bit sounding table per
// port feeds the hanging-note guard: allNotesOff(port) / allNotesOff() send Note Off for every sounding note (stop,
// track mode change, port change) and drop their pending offs. A note that is still sounding when the same note
// starts again is ended first (Note Off, then Note On, same time).
#pragma once

#include "core/util/midi/MidiWire.h"
#include "engine/EngineApi.h"

#include <array>
#include <cstdint>

namespace engine::midi
{
class MidiOutScheduler
{
public:
    static constexpr int kPorts = core::midi::kMaxPorts;
    static constexpr int kMaxBlockEvents = 512; // per block (16 tracks × 4 notes × 2 ports + clock + CCs, with room)
    static constexpr int kMaxPendingOffs = 1024;

    void attach(core::midi::MidiHub* hub) noexcept { hub_ = hub; }
    void reset() noexcept; // forget everything (prepare): no messages

    // Block frame range of the messages being collected: [blockStart, blockStart + 64).
    void beginBlock(std::int64_t blockStart) noexcept { blockStart_ = blockStart; }
    // owner = track 0–15 (its notes end on that track's mode change), −1 = none.
    void note(int port, int ch, int note, int vel, std::int64_t sampleTime, std::int64_t lengthSamples,
              int owner = -1) noexcept;
    void noteOff(int port, int ch, int note, std::int64_t sampleTime) noexcept;
    void cc(int port, int ch, int cc, int value, std::int64_t sampleTime) noexcept;
    void message(int port, std::uint8_t b0, std::uint8_t b1, std::uint8_t b2, int size, std::int64_t sampleTime) noexcept;
    void allNotesOff(int port) noexcept;   // hanging-note guard: every sounding note of the port, at the block start
    void allNotesOff() noexcept;           // every port
    void trackNotesOff(int track) noexcept; // the sounding notes a track started (its mode left MIDI)
    // The pending Note Offs due in this block, then the block's messages sorted by time → the hub.
    void endBlock() noexcept;

    bool sounding(int port, int ch, int note) const noexcept
    {
        return (sounding_[idx(port, ch)][(note >> 5) & 3] >> (note & 31) & 1u) != 0;
    }
    int soundingCount() const noexcept;
    std::uint64_t dropped() const noexcept { return dropped_; } // block full / hub full
    std::uint64_t sent() const noexcept { return sent_; }
    // The block's messages before endBlock() (tests).
    int pendingCount() const noexcept { return numEvents_; }

private:
    static std::size_t idx(int port, int ch) noexcept
    {
        return static_cast<std::size_t>(((port & (kPorts - 1)) << 4) | (ch & 15));
    }
    void push(const core::midi::MidiOutEvent& e) noexcept;
    void setSounding(int port, int ch, int note, bool on) noexcept;
    void removePendingOff(int port, int ch, int note) noexcept;

    struct PendingOff
    {
        std::int64_t at;
        std::uint8_t port, ch, note;
        std::int8_t owner;
    };
    core::midi::MidiHub* hub_ = nullptr;
    std::int64_t blockStart_ = 0;
    std::array<core::midi::MidiOutEvent, kMaxBlockEvents> events_;
    std::array<PendingOff, kMaxPendingOffs> offs_;
    int numEvents_ = 0, numOffs_ = 0;
    std::array<std::array<std::uint32_t, 4>, kPorts * 16> sounding_ {};
    std::uint64_t dropped_ = 0, sent_ = 0;
};
} // namespace engine::midi
