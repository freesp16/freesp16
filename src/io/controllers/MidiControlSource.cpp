#include "io/controllers/MidiControlSource.h"

#include <chrono>

namespace io::controllers
{
namespace
{
double steadyMs() noexcept
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}
} // namespace

MidiControlSource::MidiControlSource(const LoadedProfile& profile, int inSlot, std::string portName,
                                     engine::TriggerSink* triggers)
    : port_(std::move(portName)), id_("midi:" + port_), profileId_(profile.id), slot_(inSlot), triggers_(triggers),
      mapper_(profile, static_cast<std::uint8_t>(kSourceMidi + (inSlot & 7))), encoder_(profile)
{
}

void MidiControlSource::armLearn() noexcept
{
    learnReady_.store(false, std::memory_order_relaxed);
    learnArmed_.store(true, std::memory_order_release);
}

bool MidiControlSource::takeLearned(LearnedMessage& out) noexcept
{
    if (! learnReady_.load(std::memory_order_acquire))
        return false;
    out = learned_;
    learnReady_.store(false, std::memory_order_relaxed);
    return true;
}

bool MidiControlSource::onMidi(const std::uint8_t* msg, int len, double hostMs) noexcept
{
    if (len <= 0)
        return false;
    messages_.fetch_add(1, std::memory_order_relaxed);
    if (learnArmed_.load(std::memory_order_acquire))
    {
        const int hi = msg[0] & 0xF0;
        const bool learnable = (hi == 0x90 && len > 2 && msg[2] > 0) || hi == 0xB0 || hi == 0xE0 || hi == 0xD0
                            || (msg[0] >= 0xFA && msg[0] <= 0xFC) || msg[0] == 0xF0;
        if (learnable)
        {
            learned_.status = msg[0] == 0xF0 && len > 4 ? 0xF0 : msg[0];
            learned_.number = msg[0] == 0xF0 && len > 4 ? msg[4] : (len > 1 ? msg[1] : 0);
            learned_.value = len > 2 ? msg[2] : 0;
            learnArmed_.store(false, std::memory_order_relaxed);
            learnReady_.store(true, std::memory_order_release);
            consumed_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    MapResult r;
    if (! mapper_.map(msg, len, hostMs, r, triggers_))
        return false;
    consumed_.fetch_add(1, std::memory_order_relaxed);
    if (r.triggered)
        triggered_.fetch_add(1, std::memory_order_relaxed);
    for (int i = 0; i < r.count; ++i)
    {
        if (queue_.push(r.events[static_cast<std::size_t>(i)]))
            events_.fetch_add(1, std::memory_order_relaxed);
        else
            dropped_.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

void MidiControlSource::onRawBytes(const std::uint8_t* bytes, int n, double hostMs) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const std::uint8_t b = bytes[i];
        if (b >= 0xF8)
            continue; // realtime bytes may sit inside a SysEx
        if (b == 0xF0)
        {
            sysex_[0] = b;
            sysexLen_ = 1;
            continue;
        }
        if (sysexLen_ < 0)
            continue;
        if (b == 0xF7)
        {
            if (sysexLen_ == 5)
            {
                sysex_[5] = b;
                onMidi(sysex_, 6, hostMs); // only MMC is ever bound; anything else maps to nothing
            }
            sysexLen_ = -1;
            continue;
        }
        if (b >= 0x80 || sysexLen_ >= 5)
        {
            sysexLen_ = -1; // another status byte, or longer than an MMC command: not ours
            continue;
        }
        sysex_[sysexLen_++] = b;
    }
}

void MidiControlSource::poll(std::vector<ControlEvent>& out)
{
    for (ControlEvent e; queue_.pop(e);)
        out.push_back(e);
}

void MidiControlSource::setLamp(Control c, std::uint32_t rgb, std::uint8_t level)
{
    encoder_.set(c, rgb, level);
}

void MidiControlSource::flush()
{
    flushAt(steadyMs());
}

void MidiControlSource::flushAt(double nowMs)
{
    if (send_)
        lampsSent_ += static_cast<std::uint64_t>(encoder_.flush(nowMs, send_));
}

MidiControlSource::Stats MidiControlSource::stats() const noexcept
{
    return {messages_.load(), consumed_.load(), events_.load(), triggered_.load(), dropped_.load(), lampsSent_};
}
} // namespace io::controllers
