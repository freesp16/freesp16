// The unit's USB-B port in USB MIDI mode while this app runs (platform/common/midi/UnitMidi.h: the g_midi2 gadget and
// the USB storage gadget g_mass_storage2 exclude each other; the unit's /root/pdj/usb_gadget.sh switches them).
//
// The official application keeps the port in MIDI mode and only exports the internal disk from its own USB storage
// screen, so the port is found in storage mode when that application was left there, and with no gadget at all when
// this app started before the unit's start-up script loaded one. Once per run, a few seconds after the first hot-plug
// pass that sees the unit (the start-up script may still be loading modules):
//   storage mode  usb_gadget.sh midi probe: unloads the storage gadget, mounts the internal disk again at /mnt/emmc
//                 (the library's INTERNAL location comes back by itself), loads g_midi2 — unless a computer has the
//                 disk (the USB device controller is "configured": a host enumerated the gadget, it may have the disk
//                 mounted): then the port is left in storage mode for this run, and the log says why
//   no gadget     modprobe g_midi2 (the internal disk is mounted: nothing to remount)
//   MIDI mode     nothing
// It runs on its own thread (the script takes a moment); MidiService's next hot-plug pass opens the USB port, on
// slot 1. The command runs in a process group of its own (a timeout kills the whole group) and inherits none of this
// app's descriptors. Quitting the app does not wait for it: the thread stops waiting (bounded) and the command is
// left to finish on its own — killing usb_gadget.sh half-way could leave the port with no gadget and the internal
// disk unmounted.
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
inline constexpr const char* kUdcClass = "/sys/class/udc";

// The gadget loaded now, from <sysModules>/g_midi2 and <sysModules>/g_mass_storage2.
UsbGadget usbGadget(const std::string& sysModules = kSysModules);
const char* toString(UsbGadget) noexcept;

// A USB host has enumerated the gadget: some <udcClass>/<controller>/state reads "configured".
bool hostConfigured(const std::string& udcClass = kUdcClass);

// The command that puts the port into MIDI mode from `now` (empty: nothing to do, or no way to do it).
std::vector<std::string> midiModeCommand(UsbGadget now, bool scriptPresent);

// Runs argv (PATH lookup) in `cwd`, in a process group of its own, with none of this process's descriptors above 2
// (they are marked close-on-exec first); waits at most timeoutMs (then kills the group). Its exit status; −1 when it
// could not run, was killed or timed out; kAbandoned when `abandon` turned true first (the command is left running).
inline constexpr int kAbandoned = -2;
int runCommand(const std::vector<std::string>& argv, const char* cwd, int timeoutMs,
               const std::atomic<bool>* abandon = nullptr);
// Marks every open descriptor above 2 close-on-exec (posix_spawn of glibc 2.13 cannot close a range).
void closeOnExecAll() noexcept;

class UsbMidiMode
{
public:
    struct Env
    {
        std::function<UsbGadget()> probe;
        std::function<bool(const char* path)> exists;
        std::function<bool()> hostAttached; // a computer has the storage gadget configured (null = never)
        // Runs the command; returns early (kAbandoned) once `abandon` is true.
        std::function<int(const std::vector<std::string>&, const std::atomic<bool>& abandon)> run;
        std::function<void(const std::string&)> log;
    };
    // The real unit: /sys/module, /sys/class/udc, usb_gadget.sh in /root/pdj (30 s at most), log lines on stdout.
    static Env unitEnv();

    explicit UsbMidiMode(Env env, int graceTicks = 3) : env_(std::move(env)), grace_(graceTicks) {}
    ~UsbMidiMode(); // stops waiting for the command (bounded), does not kill it
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
    std::atomic<bool> settled_{false}, abandon_{false};
    std::thread worker_;
};
} // namespace platform::unitmidi
