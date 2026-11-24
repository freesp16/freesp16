// ALSA raw MIDI ports (what knows the machine lives in platform/). Compiled for the device and
// the Linux desktop (platform/common, LINUX_ONLY), through the existing libasound dependency — no new library.
//
// The device (platform/common/midi/UnitMidi.h, from the unit's GPL kernel sources): the DIN sockets are card
// "snddinmidi" (snd-din-midi, UART2), the USB-B MIDI gadget card "gmidi" (g_midi2), mutually exclusive with USB storage
// mode (docs/storage.md §2) — so the USB port vanishes while the unit exports its storage and comes back after:
// MidiService's 1 s hot-plug pass reconnects it by name. ALSA numbers the gadget's card before the DIN card, so the
// ports are not placed by card order: DIN always takes slot 0 (port A, the default SYNC PORT) and USB slot 1 (port B,
// kept for it in storage mode). The DIN output
//   - gives the OUT socket to UART2 when it opens (/proc/midi_through '0': the official application may have left it
//     on THRU, and then nothing this app sends would leave the unit) and again when it closes;
//   - switches the socket to hardware THRU when port A is set to THRU A (MidiService: DIN IN → DIN OUT, every byte);
//   - writes the real-time bytes (clock, start, continue, stop) through /dev/din_midi's priority queue, so a tick never
//     waits behind note bytes in the raw MIDI buffer (31.25 kbit/s: 0.32 ms a byte); a song position is drained out
//     before the Continue that follows it can overtake it. Without /dev/din_midi (or when its ioctl fails for good)
//     everything goes through the raw MIDI device; a full queue sends that one byte the raw way
//     (unitmidi::queueRealtime).
// The USB port is put into USB MIDI mode once per run if the unit was left in USB storage mode (UsbMidiMode.h; the
// setting midi_usb=0 leaves it as found).
// The desktop: every raw MIDI device of the sound cards (USB interfaces, snd-virmidi).
//
// Port name = "<card name> <device>:<subdevice> <subdevice name>" (the reconnect key; the unit's own cards read "MIDI
// DIN" / "MIDI USB"); id = "hw:C,D,S".
#include "io/midi/MidiPort.h"
#include "io/midi/MidiSettings.h"
#include "platform/common/midi/UnitMidi.h"
#include "platform/common/midi/UsbMidiMode.h"

#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace platform
{
namespace
{
class AlsaInput final : public io::midi::MidiInput
{
public:
    explicit AlsaInput(snd_rawmidi_t* h) : h_(h) {}
    ~AlsaInput() override { snd_rawmidi_close(h_); }
    int read(std::uint8_t* buf, int cap, int timeoutMs) override
    {
        pollfd fds[4];
        const int n = snd_rawmidi_poll_descriptors(h_, fds, 4);
        if (n <= 0)
            return -1;
        const int r = ::poll(fds, static_cast<nfds_t>(n), timeoutMs);
        if (r < 0)
            return 0; // EINTR
        if (r == 0)
            return 0;
        unsigned short revents = 0;
        if (snd_rawmidi_poll_descriptors_revents(h_, fds, static_cast<unsigned>(n), &revents) < 0 ||
            (revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
            return -1; // unplugged
        if ((revents & POLLIN) == 0)
            return 0;
        const ssize_t got = snd_rawmidi_read(h_, buf, static_cast<size_t>(cap));
        if (got == -EAGAIN)
            return 0;
        return got < 0 ? -1 : static_cast<int>(got);
    }

private:
    snd_rawmidi_t* h_;
};

// One whole message into a raw MIDI output opened non-blocking + append (all or nothing): never blocks — a USB host
// that stops reading fills the gadget's buffer, and a blocking write would then hold up every port's clock.
io::midi::WriteResult writeRaw(snd_rawmidi_t* h, const std::uint8_t* bytes, int size) noexcept
{
    const ssize_t r = snd_rawmidi_write(h, bytes, static_cast<size_t>(size));
    if (r == size)
        return io::midi::WriteResult::Written;
    return r == -EAGAIN || r == -EINTR || r == -EBUSY || r >= 0 ? io::midi::WriteResult::Busy
                                                                : io::midi::WriteResult::Gone;
}

class AlsaOutput final : public io::midi::MidiOutput
{
public:
    explicit AlsaOutput(snd_rawmidi_t* h) : h_(h) {}
    ~AlsaOutput() override { snd_rawmidi_close(h_); }
    io::midi::WriteResult write(const std::uint8_t* bytes, int size) override { return writeRaw(h_, bytes, size); }

private:
    snd_rawmidi_t* h_;
};

// The unit's DIN output (UnitMidi.h): the raw MIDI device + the /dev/din_midi priority queue + the OUT/THRU switch.
class DinOutput final : public io::midi::MidiOutput
{
public:
    explicit DinOutput(snd_rawmidi_t* h) : h_(h)
    {
        unitmidi::setHardwareThru(false); // the OUT socket is ours (the official application may have left THRU on)
        // After the raw MIDI output is open: the driver refuses the ioctl (−ESRCH) without it.
        fd_ = ::open(unitmidi::kDinMidiDevice, O_RDWR | O_CLOEXEC);
    }
    ~DinOutput() override
    {
        if (fd_ >= 0)
            ::close(fd_);
        unitmidi::setHardwareThru(false); // leave the OUT socket as an output
        snd_rawmidi_close(h_);
    }
    io::midi::WriteResult write(const std::uint8_t* bytes, int size) override
    {
        if (fd_ >= 0 && size == 1)
        {
            const int code = unitmidi::realtimeCode(bytes[0]);
            if (code >= 0)
            {
                const auto r = unitmidi::queueRealtime(fd_, code);
                if (r == unitmidi::QueueResult::Queued)
                    return io::midi::WriteResult::Written;
                if (r == unitmidi::QueueResult::Disable)
                {
                    ::close(fd_); // the queue does not work: everything through the raw MIDI device from now on
                    fd_ = -1;
                }
            }
        }
        const auto r = writeRaw(h_, bytes, size);
        // A song position on the wire before the Start / Continue / clock that follows it in the priority queue (the
        // drain waits, also on a non-blocking handle: the clock thread sends nothing else meanwhile, ~1 ms).
        if (r == io::midi::WriteResult::Written && fd_ >= 0 && bytes[0] == 0xF2)
            snd_rawmidi_drain(h_);
        return r;
    }
    bool setHardwareThru(bool on) override { return unitmidi::setHardwareThru(on); }

private:
    snd_rawmidi_t* h_;
    int fd_ = -1; // /dev/din_midi; −1 = none (the desktop, an old kernel) or refused
};

void listDirection(snd_ctl_t* ctl, int card, int device, const std::string& cardName, unitmidi::UnitPort unit,
                   snd_rawmidi_stream_t stream, std::vector<io::midi::PortInfo>& out)
{
    snd_rawmidi_info_t* info = nullptr;
    snd_rawmidi_info_alloca(&info);
    snd_rawmidi_info_set_device(info, static_cast<unsigned>(device));
    snd_rawmidi_info_set_stream(info, stream);
    snd_rawmidi_info_set_subdevice(info, 0);
    if (snd_ctl_rawmidi_info(ctl, info) < 0)
        return;
    const unsigned subs = snd_rawmidi_info_get_subdevices_count(info);
    for (unsigned s = 0; s < subs && s < 16; ++s)
    {
        snd_rawmidi_info_set_subdevice(info, s);
        if (snd_ctl_rawmidi_info(ctl, info) < 0)
            continue;
        char id[48];
        std::snprintf(id, sizeof id, "hw:%d,%d,%u", card, device, s);
        // The unit's two ports by what they are, not by their kernel drivers: the USB-B MIDI gadget = MIDI USB, the
        // DIN sockets = MIDI DIN; a further device / subdevice of the same card gets its number appended.
        const char* friendly = unit == unitmidi::UnitPort::Usb   ? unitmidi::kUsbPortName
                               : unit == unitmidi::UnitPort::Din ? unitmidi::kDinPortName
                                                                 : nullptr;
        const bool first = device == 0 && s == 0;
        std::string name;
        if (friendly != nullptr)
        {
            name = friendly;
            if (device > 0 || s > 0)
                name += " " + std::to_string(device * 16 + static_cast<int>(s) + 1);
        }
        else
        {
            const char* sub = snd_rawmidi_info_get_subdevice_name(info);
            name = cardName + " " + std::to_string(device) + ":" + std::to_string(s);
            if (sub != nullptr && *sub != '\0')
                name += std::string(" ") + sub;
        }
        bool merged = false;
        for (auto& p : out)
            if (p.id == id)
            {
                (stream == SND_RAWMIDI_STREAM_INPUT ? p.input : p.output) = true;
                merged = true;
            }
        if (! merged)
        {
            io::midi::PortInfo p {id, name, stream == SND_RAWMIDI_STREAM_INPUT, stream == SND_RAWMIDI_STREAM_OUTPUT,
                                  first ? unitmidi::fixedSlot(unit) : -1};
            out.push_back(p);
        }
    }
}

class AlsaRawMidiProvider final : public io::midi::PortProvider
{
public:
    std::string_view id() const override { return "alsa-rawmidi"; }
    std::vector<io::midi::PortInfo> list() override
    {
        std::vector<io::midi::PortInfo> out;
        snd_ctl_card_info_t* info = nullptr;
        snd_ctl_card_info_alloca(&info);
        int card = -1;
        while (snd_card_next(&card) >= 0 && card >= 0)
        {
            char hw[16];
            std::snprintf(hw, sizeof hw, "hw:%d", card);
            snd_ctl_t* ctl = nullptr;
            if (snd_ctl_open(&ctl, hw, SND_CTL_NONBLOCK) < 0)
                continue;
            std::string cardId, name = hw;
            if (snd_ctl_card_info(ctl, info) >= 0)
            {
                cardId = snd_ctl_card_info_get_id(info);
                if (const char* n = snd_ctl_card_info_get_name(info); n != nullptr && *n != '\0')
                    name = n;
            }
            const auto unit = unitmidi::classify(cardId, name);
            if (unit != unitmidi::UnitPort::None)
                unitSeen_ = true;
            int device = -1;
            while (snd_ctl_rawmidi_next_device(ctl, &device) >= 0 && device >= 0)
            {
                listDirection(ctl, card, device, name, unit, SND_RAWMIDI_STREAM_INPUT, out);
                listDirection(ctl, card, device, name, unit, SND_RAWMIDI_STREAM_OUTPUT, out);
            }
            snd_ctl_close(ctl);
        }
        if (unitSeen_ && io::midi::setting("midi_usb", "1") != "0") // USB-B in MIDI mode (once per run)
        {
            if (! usbMode_)
                usbMode_ = std::make_unique<unitmidi::UsbMidiMode>(unitmidi::UsbMidiMode::unitEnv());
            usbMode_->tick();
        }
        // The unit's ports first (DIN, then USB), whatever their card numbers.
        std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
            return static_cast<unsigned>(a.slot) < static_cast<unsigned>(b.slot); // −1 (any other port) sorts last
        });
        return out;
    }
    // On the unit (its DIN card is there for good): slots 0 and 1 stay DIN's and USB's, the gadget in storage mode too.
    std::uint32_t reservedSlots() const override
    {
        return unitSeen_ ? (1u << unitmidi::fixedSlot(unitmidi::UnitPort::Din)) |
                               (1u << unitmidi::fixedSlot(unitmidi::UnitPort::Usb))
                         : 0u;
    }
    std::string_view slotName(int slot) const override // the unit's ports keep their names while absent
    {
        if (! unitSeen_)
            return {};
        return slot == unitmidi::fixedSlot(unitmidi::UnitPort::Din)   ? unitmidi::kDinPortName
               : slot == unitmidi::fixedSlot(unitmidi::UnitPort::Usb) ? unitmidi::kUsbPortName
                                                                      : std::string_view();
    }
    std::unique_ptr<io::midi::MidiInput> openInput(const io::midi::PortInfo& p, std::string& error) override
    {
        snd_rawmidi_t* h = nullptr;
        const int r = snd_rawmidi_open(&h, nullptr, p.id.c_str(), SND_RAWMIDI_NONBLOCK);
        if (r < 0)
        {
            error = snd_strerror(r);
            return nullptr;
        }
        return std::make_unique<AlsaInput>(h);
    }
    std::unique_ptr<io::midi::MidiOutput> openOutput(const io::midi::PortInfo& p, std::string& error) override
    {
        snd_rawmidi_t* h = nullptr;
        // Non-blocking: a blocking open waits for as long as another process holds the port (the app would hang at
        // start-up), and a blocking write waits for room — a USB host that does not read the port would stall the
        // MIDI clock thread (writeRaw). Append: a message goes into the buffer whole or not at all.
        const int r = snd_rawmidi_open(nullptr, &h, p.id.c_str(), SND_RAWMIDI_NONBLOCK | SND_RAWMIDI_APPEND);
        if (r < 0)
        {
            error = snd_strerror(r);
            return nullptr;
        }
        if (p.slot == unitmidi::fixedSlot(unitmidi::UnitPort::Din)) // list() gives slot 0 to the DIN port only
            return std::make_unique<DinOutput>(h);
        return std::make_unique<AlsaOutput>(h);
    }

private:
    bool unitSeen_ = false;
    std::unique_ptr<unitmidi::UsbMidiMode> usbMode_; // the unit only
};

std::unique_ptr<io::midi::PortProvider> makeAlsaRawMidi() { return std::make_unique<AlsaRawMidiProvider>(); }
} // namespace
} // namespace platform

APP_REGISTER_MIDI_PROVIDER("alsa-rawmidi", &platform::makeAlsaRawMidi)
