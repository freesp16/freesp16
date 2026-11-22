// MIDI ports behind an interface (spec 02-features/midi.md MID-001…004; what knows a device lives in
// platform/). A PortProvider enumerates and opens ports by name; io/midi (MidiService) owns the threads,
// the hot-plug poll and the reconnect-by-name, the platform layer only supplies providers:
//
//   "alsa-rawmidi"  platform/common/midi (the device and the Linux desktop): ALSA raw MIDI through libasound — on the
//                   device the DIN driver and the USB MIDI gadget (research docs/midi-implementation.md, findings.md
//                   "snd-din-midi"; storage.md: USB MIDI and USB storage are mutually exclusive gadget modes, so the
//                   USB port appears and disappears — hot-plug)
//   "virtual"       io/midi/VirtualPort (every platform): a scripted input + a recording output for tests and
//                   scripts/smoke/93-midi.sh (setting midi_virtual=<script>)
//
// Providers register with APP_REGISTER_MIDI_PROVIDER(id, factory) from the library that holds them (linked
// whole-archive, like every APP_REGISTER_* registrar).
#pragma once

#include "core/util/Registrar.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace io::midi
{
struct PortInfo
{
    std::string id;   // provider-specific address ("hw:1,0,0")
    std::string name; // stable display name, the reconnect key ("USB MIDI 1"); unique per provider
    bool input = false, output = false;
};

class MidiInput
{
public:
    virtual ~MidiInput() = default;
    // Blocks for at most timeoutMs; returns the bytes read (0 = timeout), −1 = the port is gone (unplugged).
    // Called from the port's own thread only.
    virtual int read(std::uint8_t* buffer, int capacity, int timeoutMs) = 0;
};

class MidiOutput
{
public:
    virtual ~MidiOutput() = default;
    // Writes one complete message (sender thread only); false = the port is gone.
    virtual bool write(const std::uint8_t* bytes, int size) = 0;
};

class PortProvider
{
public:
    virtual ~PortProvider() = default;
    virtual std::string_view id() const = 0;
    virtual std::vector<PortInfo> list() = 0; // message thread (hot-plug poll, every 1 s)
    virtual std::unique_ptr<MidiInput> openInput(const PortInfo&, std::string& error) = 0;
    virtual std::unique_ptr<MidiOutput> openOutput(const PortInfo&, std::string& error) = 0;
};

using PortProviderFactory = std::unique_ptr<PortProvider> (*)();
struct PortProviderEntry
{
    std::string_view id;
    PortProviderFactory create;
};
bool registerPortProvider(std::string_view id, PortProviderFactory);
std::vector<PortProviderEntry> portProviders(); // sorted by id
} // namespace io::midi

#define APP_REGISTER_MIDI_PROVIDER(id, factory) APP_STATIC_REGISTRAR(::io::midi::registerPortProvider(id, factory))
