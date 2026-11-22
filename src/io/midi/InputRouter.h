// The controller hook of the MIDI ports (control-surface.md UI-130/136): one
// process-wide InputRouter (io::controllers::ControllerHub) sees every input port's messages on the port's reader
// thread before the hub does, and consumes the ones a controller profile uses — they reach neither the engine's MIDI
// input (MID-*) nor the merge / thru forwarder. MidiService tells it which ports opened and closed (message thread)
// and hands it the sender for lamp output; a router installed after ports opened is told about them on the next
// refresh. With no router installed the ports behave exactly as before.
//
// Lifetime: the router must outlive every MidiService that may call it (the app's is a process-lifetime object).
#pragma once

#include "core/util/midi/MidiWire.h"

#include <cstdint>
#include <string_view>

namespace io::midi
{
class MidiSender;

class InputRouter
{
public:
    virtual ~InputRouter() = default;
    // Message thread (MidiService::refresh / stop).
    virtual void inputOpened(int slot, std::string_view name) = 0;
    virtual void inputClosed(int slot) = 0; // the port's thread has ended
    virtual void outputOpened(int slot, std::string_view name, MidiSender& sender) = 0;
    virtual void outputClosed(int slot) = 0;
    // The port's reader thread: the raw bytes as read (before parsing; for SysEx such as MMC, which the parser skips)
    // and then each parsed message. True = consumed. noexcept, no allocation, no lock.
    virtual void rawBytes(int slot, const std::uint8_t* bytes, int size, std::int64_t hostNs) noexcept = 0;
    virtual bool message(int slot, const core::midi::MidiEvent& e) noexcept = 0;
};

void setInputRouter(InputRouter*) noexcept; // message thread
InputRouter* inputRouter() noexcept;        // any thread
} // namespace io::midi
