#include "platform/common/midi/UnitMidi.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace platform::unitmidi
{
UnitPort classify(std::string_view id, std::string_view name) noexcept
{
    if (id == "snddinmidi" || name == "snd-din-midi")
        return UnitPort::Din;
    if (id == "gmidi" || name == "g_midi")
        return UnitPort::Usb;
    return UnitPort::None;
}

int fixedSlot(UnitPort p) noexcept { return p == UnitPort::Din ? 0 : p == UnitPort::Usb ? 1 : -1; }

int realtimeCode(std::uint8_t status) noexcept
{
    switch (status)
    {
        case 0xF8: return 0; // timing clock
        case 0xFA: return 1; // start
        case 0xFB: return 2; // continue
        case 0xFC: return 3; // stop
        case 0xFE: return 4; // active sensing
        case 0xFF: return 5; // system reset
        default: return -1;
    }
}

int ioctlSubmit(int fd, std::uint32_t* code) noexcept
{
    return ::ioctl(fd, kDinRealtimeIoctl, code);
}

QueueResult queueRealtime(int fd, int code, QueueSubmit submit) noexcept
{
    auto request = static_cast<std::uint32_t>(code);
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        if (submit(fd, &request) == 0)
            return QueueResult::Queued;
        const int e = errno;
        if (e == EINTR)
            continue;
        if (e == EAGAIN || e == EWOULDBLOCK || e == EBUSY || e == ENOBUFS)
            return QueueResult::RawThisTime;
        return QueueResult::Disable;
    }
    return QueueResult::RawThisTime; // interrupted again and again: this byte the other way
}

bool setHardwareThru(bool on, const char* path) noexcept
{
    const int fd = ::open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    const char c = on ? '1' : '0';
    const bool ok = ::write(fd, &c, 1) == 1;
    ::close(fd);
    return ok;
}
} // namespace platform::unitmidi
