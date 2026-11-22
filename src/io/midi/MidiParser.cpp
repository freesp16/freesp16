#include "io/midi/MidiParser.h"

namespace io::midi
{
int MidiParser::dataBytes(std::uint8_t s) noexcept
{
    if (s < 0x80)
        return -1;
    if (s < 0xF0)
    {
        const int t = s & 0xF0;
        return t == 0xC0 || t == 0xD0 ? 1 : 2;
    }
    switch (s)
    {
        case 0xF1: return 1; // MTC quarter frame
        case 0xF2: return 2; // song position
        case 0xF3: return 1; // song select
        case 0xF6: return 0; // tune request
        default: return s >= 0xF8 ? 0 : -1;
    }
}

bool MidiParser::feed(std::uint8_t b, core::midi::MidiEvent& out) noexcept
{
    if (b >= 0xF8) // real time: complete in itself, does not disturb running status or SysEx
    {
        if (b == 0xF9 || b == 0xFD)
            return false; // undefined
        out.size = 1;
        out.bytes[0] = b;
        out.bytes[1] = out.bytes[2] = 0;
        return true;
    }
    if (b == 0xF0)
    {
        sysex_ = true;
        status_ = 0;
        have_ = 0;
        return false;
    }
    if (b == 0xF7)
    {
        sysex_ = false;
        return false;
    }
    if (b >= 0x80)
    {
        sysex_ = false;
        const int n = dataBytes(b);
        have_ = 0;
        if (n < 0)
        {
            status_ = 0;
            return false;
        }
        if (n == 0) // tune request
        {
            status_ = 0;
            out.size = 1;
            out.bytes[0] = b;
            out.bytes[1] = out.bytes[2] = 0;
            return true;
        }
        status_ = b; // system common clears running status after it completes (below)
        return false;
    }
    // A data byte.
    if (sysex_ || status_ == 0)
        return false;
    data_[have_++] = b;
    const int need = dataBytes(status_);
    if (have_ < need)
        return false;
    out.size = static_cast<std::uint8_t>(1 + need);
    out.bytes[0] = status_;
    out.bytes[1] = data_[0];
    out.bytes[2] = need > 1 ? data_[1] : 0;
    have_ = 0;
    if (status_ >= 0xF0)
        status_ = 0; // no running status for system common
    return true;
}
} // namespace io::midi
