// The unit's USB-B port in USB MIDI mode while this app runs (platform/common/midi/UnitMidi.h: the g_midi2 gadget and
// the USB storage gadget g_mass_storage2 exclude each other; the unit's /root/pdj/usb_gadget.sh switches them).
//
// The official application keeps the port in MIDI mode and only exports the internal disk from its own USB storage
// screen, so the port is found in storage mode when that application was left there, and with no gadget at all when
// this app started before the unit's start-up script loaded one. Once per run, a few seconds after the first hot-plug
// pass that sees the unit (the start-up script may still be loading modules):
//   storage mode  usb_gadget.sh midi probe: unloads the storage gadget (a computer on USB-B sees the disk go away),
//                 mounts the internal disk again at /mnt/emmc (the library's INTERNAL location comes back by itself),
//                 loads g_midi2
//   no gadget     modprobe g_midi2 (the internal disk is mounted: nothing to remount)
//   MIDI mode     nothing
// It runs on its own thread (the script takes a moment); MidiService's next hot-plug pass opens the USB port, on
// slot 1.
// Only once: a later switch to storage mode (usb_gadget.sh by hand, to copy files) is left alone. The setting
// midi_usb=0 leaves the port as it is found.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace platform::unitmidi
{
enum class UsbGadget : std::uint8_t { None, Midi, Storage };

inline constexpr const char* kSysModules = "/sys/module";
inline constexpr const char* kUsbGadgetScript = "/root/pdj/usb_gadget.sh";

// The gadget loaded now, from <sysModules>/g_midi2 and <sysModules>/g_mass_storage2.
UsbGadget usbGadget(const std::string& sysModules = kSysModules);
const char* toString(UsbGadget) noexcept;

// The command that puts the port into MIDI mode from `now` (empty: nothing to do, or no way to do it).
std::vector<std::string> midiModeCommand(UsbGadget now, bool scriptPresent);

// Runs argv (PATH lookup) in `cwd`, waits at most timeoutMs (then kills it); its exit status, −1 when it could not run,
// was killed or timed out.
int runCommand(const std::vector<std::string>& argv, const char* cwd, int timeoutMs);

class UsbMidiMode
{
public:
    struct Env
    {
        std::function<UsbGadget()> probe;
        std::function<bool(const char* path)> exists;
        std::function<int(const std::vector<std::string>&)> run;
        std::function<void(const std::string&)> log;
    };
    // The real unit: /sys/module, usb_gadget.sh in /root/pdj (30 s at most), log lines on stdout.
    static Env unitEnv();

    explicit UsbMidiMode(Env env, int graceTicks = 3) : env_(std::move(env)), grace_(graceTicks) {}
    ~UsbMidiMode();
    UsbMidiMode(const UsbMidiMode&) = delete;
    UsbMidiMode& operator= (const UsbMidiMode&) = delete;

    // Message thread, once per hot-plug pass on the unit.
    void tick();
    bool settled() const noexcept { return settled_.load(); } // decided, and the command (if any) finished
    void wait();                                               // joins the command's thread (tests)

private:
    Env env_;
    int grace_, ticks_ = 0;
    bool decided_ = false;
    std::atomic<bool> settled_ {false};
    std::thread worker_;
};
} // namespace platform::unitmidi
