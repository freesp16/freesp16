// The reference unit's own MIDI hardware, as its kernel exposes it (GPL kernel sources of the unit: sound/drivers/
// din-midi.c, arch/arm/mach-mx6/midi_thru.c, drivers/usb/gadget2/gmidi.c; the module set apl_start loads before the
// official application starts, so it is loaded when this app runs):
//
//   DIN sockets   snd-din-midi on UART2 at 31,250 baud: ALSA card "snddinmidi" (name "snd-din-midi"), raw MIDI
//                 device 0, plus the char device /dev/din_midi (155:0) whose ioctl DIN_MIDI_IOC_WD_SYS_RT_MES
//                 (_IOW('p', 0, __u32)) queues a real-time byte ahead of the raw MIDI buffer — codes 0 F8, 1 FA, 2 FB,
//                 3 FC, 4 FE, 5 FF; any other code is −EINVAL. The raw MIDI output must be open first, else −ESRCH.
//   OUT / THRU    midi_thru: writing '1' to /proc/midi_through muxes the DIN OUT socket onto DIN IN in hardware (no
//                 CPU, every byte including SysEx); '0' gives the socket back to UART2 (what this app sends). The state
//                 survives the official application: it may have left the socket on THRU.
//   USB-B         the g_midi2 gadget: ALSA card "gmidi" (name "g_midi"), one port each way. It and the USB storage
//                 gadget are mutually exclusive (the unit's usb_gadget.sh switches them), so this port comes and goes.
//
// Never use /dev/ttymxc1 (the same UART) and never read GPIO 1_5 through /dev/gpiodrv (a read turns the MIDI PASS
// pin into an input): /proc/midi_through is the one switch for the socket.
#pragma once

#include <cstdint>
#include <string_view>

namespace platform::unitmidi
{
enum class UnitPort : std::uint8_t { None, Din, Usb };

// Which of the unit's ports an ALSA card is, by its id ("snddinmidi", "gmidi") or its name ("snd-din-midi", "g_midi").
UnitPort classify(std::string_view cardId, std::string_view cardName) noexcept;

// The slot the unit's ports always take (MidiService): DIN = 0 (port A, the default SYNC PORT), USB = 1 (port B, kept
// for it while the unit is in USB storage mode); −1 = any other port.
int fixedSlot(UnitPort) noexcept;
// Their port names (the reconnect key, and what UTILITY shows): by what they are, not by their kernel drivers.
inline constexpr const char* kDinPortName = "MIDI DIN";
inline constexpr const char* kUsbPortName = "MIDI USB";

inline constexpr const char* kDinMidiDevice = "/dev/din_midi";
inline constexpr const char* kMidiThroughPath = "/proc/midi_through";
inline constexpr unsigned long kDinRealtimeIoctl = 0x40047000UL; // DIN_MIDI_IOC_WD_SYS_RT_MES = _IOW('p', 0, __u32)

// The /dev/din_midi code of a real-time status byte (F8 0, FA 1, FB 2, FC 3, FE 4, FF 5); −1 = not one it takes.
int realtimeCode(std::uint8_t status) noexcept;

// Writes '1' (hardware THRU: DIN IN → DIN OUT) or '0' (the socket is UART2's) to `path`; false when it cannot.
bool setHardwareThru(bool on, const char* path = kMidiThroughPath) noexcept;
} // namespace platform::unitmidi
