// MIDI ports, their threads and hot-plug (spec 02-features/midi.md MID-001/002/003, NFR-013,
// 01-architecture/threading.md "MIDI input (per device)").
//
//   input ports   up to 8 slots; each open input has its own thread: read → parse (MidiParser) → stamp
//                 CLOCK_MONOTONIC → MidiHub::pushIn (to the engine) and pushForward (to the sender's merge / thru).
//                 Nothing else happens there. Role::MidiIn (core/util/ThreadRole.h), best effort.
//   output ports  up to 8 slots, written by the one MidiSender thread
//   hot-plug      refresh() (the message thread calls poll() every frame; it refreshes every 1 s) lists every
//                 provider's ports and reconnects them by name: a name keeps the slot it first got, so a replugged
//                 device comes back on its own port (A/B, offsets, modes stay); a vanished port is closed and the hub
//                 told (the engine's hanging-note guard). The audio thread is never touched (NFR-013).
//   hardware THRU poll(), every frame: an output port in THRU A / THRU B whose source input is the same physical port
//                 (the unit's DIN IN → DIN OUT) is switched to THRU in hardware (MidiOutput::setHardwareThru) and the
//                 sender stops forwarding to it; any other mode gives the socket back to the port's own output.
// Slots: a port with a fixed slot (PortInfo::slot: the unit's DIN = slot 0 = port A, its USB gadget = slot 1 = port
// B) always takes it, and the providers' reserved slots stay free for them while they are absent (USB storage mode);
// every other port takes the first free slot in the order ports are first seen.
#pragma once

#include "core/util/midi/MidiWire.h"
#include "io/midi/MidiParser.h"
#include "io/midi/MidiPort.h"
#include "io/midi/MidiSender.h"

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace io::midi
{
class InputRouter; // io/midi/InputRouter.h

class MidiService
{
public:
    static constexpr int kPorts = core::midi::kMaxPorts;
    static constexpr double kRefreshMs = 1000.0;

    MidiService(core::midi::MidiHub& hub, std::vector<std::unique_ptr<PortProvider>> providers);
    ~MidiService();
    MidiService(const MidiService&) = delete;
    MidiService& operator= (const MidiService&) = delete;

    bool start(std::string& info); // opens the ports present now, starts the sender
    void stop();
    void poll(double nowMs);       // message thread, every frame: a refresh every kRefreshMs
    void refresh();                // message thread: list, close vanished ports, (re)open present ones by name

    struct Slot
    {
        std::string name, provider;
        bool connected = false;
    };
    const std::array<Slot, kPorts>& inputs() const noexcept { return inSlots_; }
    const std::array<Slot, kPorts>& outputs() const noexcept { return outSlots_; }
    // The name of slot `slot` for the UI: its port's (the output's, else the input's), else the name of the fixed-slot
    // port a provider keeps it for (the unit's USB port in storage mode: "MIDI USB"); "" = a free slot.
    std::string_view slotName(int slot) const; // valid until the next refresh()
    std::uint64_t messagesIn() const noexcept { return messagesIn_.load(); }
    std::uint64_t reconnects() const noexcept { return reconnects_; }
    // Output `slot` passes its own input on in hardware (THRU A / B on the unit's DIN port).
    bool hardwareThru(int slot) const noexcept
    {
        return slot >= 0 && slot < kPorts && hardwareThru_[static_cast<std::size_t>(slot)];
    }
    MidiSender& sender() noexcept { return sender_; }
    std::string describe() const;

private:
    struct Input
    {
        std::unique_ptr<MidiInput> port;
        std::thread thread;
        std::atomic<bool> quit {false}, gone {false};
    };
    void closeInput(int slot);
    void closeOutput(int slot);
    int slotFor(std::array<Slot, kPorts>& slots, const std::string& provider, const std::string& name, int fixed);
    void inputLoop(int slot, MidiInput* port, Input* state);
    void applyHardwareThru(); // message thread: the THRU modes onto the ports' hardware switches

    core::midi::MidiHub& hub_;
    std::vector<std::unique_ptr<PortProvider>> providers_;
    std::array<Slot, kPorts> inSlots_ {}, outSlots_ {};
    std::array<Input, kPorts> inputs_ {};
    std::array<std::unique_ptr<MidiOutput>, kPorts> outputs_ {};
    std::array<bool, kPorts> thruAsked_ {};    // the switch state asked of each open output
    std::array<bool, kPorts> hardwareThru_ {}; // … and whether the port did it
    std::uint32_t reserved_ = 0;               // slots kept for fixed-slot ports (sticky)
    MidiSender sender_;
    double lastRefreshMs_ = -1.0e18;
    InputRouter* notified_ = nullptr; // the router told about the open ports
    std::atomic<std::uint64_t> messagesIn_ {0};
    std::uint64_t reconnects_ = 0;
    bool started_ = false;
};
} // namespace io::midi
