// MIDI byte-stream parser for the port threads (spec 01-architecture/threading.md: the MIDI input thread
// parses and timestamps only). Running status, real-time bytes (F8…FF) interleaved anywhere (also inside a message
// and inside SysEx), SysEx skipped (not received), undefined bytes dropped. No allocation; one parser per port.
#pragma once

#include "core/util/midi/MidiWire.h"

#include <cstdint>

namespace io::midi
{
class MidiParser
{
public:
    // Feeds one byte; true when `out` holds a complete message (port and hostTimeNs are left to the caller).
    bool feed(std::uint8_t b, core::midi::MidiEvent& out) noexcept;
    void reset() noexcept
    {
        status_ = 0;
        have_ = 0;
        sysex_ = false;
    }
    // Number of data bytes a status byte takes (channel / system common); −1 = not a message start.
    static int dataBytes(std::uint8_t status) noexcept;

private:
    std::uint8_t status_ = 0; // running status (channel messages) or the pending system common status
    std::uint8_t data_[2] {};
    int have_ = 0;
    bool sysex_ = false;
};
} // namespace io::midi
