#include "engine/midi/MidiOutScheduler.h"

namespace engine::midi
{
void MidiOutScheduler::reset() noexcept
{
    numEvents_ = numOffs_ = 0;
    for (auto& s : sounding_)
        s = {};
}

void MidiOutScheduler::push(const core::midi::MidiOutEvent& e) noexcept
{
    if (numEvents_ == kMaxBlockEvents)
    {
        ++dropped_;
        return;
    }
    events_[static_cast<std::size_t>(numEvents_++)] = e;
}

void MidiOutScheduler::message(int port, std::uint8_t b0, std::uint8_t b1, std::uint8_t b2, int size, std::int64_t t,
                               std::int16_t frac) noexcept
{
    if (port < 0 || port >= kPorts || size < 1 || size > 3)
        return;
    core::midi::MidiOutEvent e;
    e.port = static_cast<std::uint8_t>(port);
    e.size = static_cast<std::uint8_t>(size);
    e.bytes[0] = b0;
    e.bytes[1] = b1;
    e.bytes[2] = b2;
    e.sampleTime = t < blockStart_ ? blockStart_ : t;
    e.frac = t < blockStart_ ? std::int16_t{0} : frac;
    push(e);
}

void MidiOutScheduler::setSounding(int port, int ch, int note, bool on) noexcept
{
    auto& w = sounding_[idx(port, ch)][(note >> 5) & 3];
    const auto bit = 1u << (note & 31);
    w = on ? (w | bit) : (w & ~bit);
}

void MidiOutScheduler::removePendingOff(int port, int ch, int note) noexcept
{
    for (int i = 0; i < numOffs_; ++i)
    {
        const auto& o = offs_[static_cast<std::size_t>(i)];
        if (o.port == port && o.ch == ch && o.note == note)
        {
            offs_[static_cast<std::size_t>(i)] = offs_[static_cast<std::size_t>(--numOffs_)];
            return;
        }
    }
}

void MidiOutScheduler::note(int port, int ch, int note, int vel, std::int64_t t, std::int64_t length,
                            int owner) noexcept
{
    if (port < 0 || port >= kPorts || note < 0 || note > 127)
        return;
    ch &= 15;
    vel = vel < 1 ? 1 : (vel > 127 ? 127 : vel);
    if (sounding(port, ch, note))
    {
        removePendingOff(port, ch, note);
        message(port, static_cast<std::uint8_t>(0x80 | ch), static_cast<std::uint8_t>(note), 0, 3, t);
    }
    message(port, static_cast<std::uint8_t>(0x90 | ch), static_cast<std::uint8_t>(note), static_cast<std::uint8_t>(vel),
            3, t);
    setSounding(port, ch, note, true);
    if (numOffs_ == kMaxPendingOffs)
    {
        // No room for the off: end the note at once rather than leave it hanging.
        message(port, static_cast<std::uint8_t>(0x80 | ch), static_cast<std::uint8_t>(note), 0, 3, t);
        setSounding(port, ch, note, false);
        ++dropped_;
        return;
    }
    offs_[static_cast<std::size_t>(numOffs_++)] = {t + (length < 1 ? 1 : length), static_cast<std::uint8_t>(port),
                                                   static_cast<std::uint8_t>(ch), static_cast<std::uint8_t>(note),
                                                   static_cast<std::int8_t>(owner)};
}

void MidiOutScheduler::noteOff(int port, int ch, int note, std::int64_t t) noexcept
{
    if (port < 0 || port >= kPorts || note < 0 || note > 127 || ! sounding(port, ch & 15, note))
        return;
    removePendingOff(port, ch & 15, note);
    message(port, static_cast<std::uint8_t>(0x80 | (ch & 15)), static_cast<std::uint8_t>(note), 0, 3, t);
    setSounding(port, ch & 15, note, false);
}

void MidiOutScheduler::cc(int port, int ch, int cc, int value, std::int64_t t) noexcept
{
    message(port, static_cast<std::uint8_t>(0xB0 | (ch & 15)), static_cast<std::uint8_t>(cc & 127),
            static_cast<std::uint8_t>(value < 0 ? 0 : (value > 127 ? 127 : value)), 3, t);
}

void MidiOutScheduler::allNotesOff(int port) noexcept
{
    if (port < 0 || port >= kPorts)
        return;
    for (int i = 0; i < numOffs_;)
        if (offs_[static_cast<std::size_t>(i)].port == port)
            offs_[static_cast<std::size_t>(i)] = offs_[static_cast<std::size_t>(--numOffs_)];
        else
            ++i;
    for (int ch = 0; ch < 16; ++ch)
    {
        auto& words = sounding_[idx(port, ch)];
        for (int w = 0; w < 4; ++w)
            for (std::uint32_t m = words[static_cast<std::size_t>(w)], b = 0; m != 0; ++b, m >>= 1)
                if (m & 1u)
                    message(port, static_cast<std::uint8_t>(0x80 | ch), static_cast<std::uint8_t>(w * 32 + static_cast<int>(b)),
                            0, 3, blockStart_);
        words = {};
    }
}

void MidiOutScheduler::allNotesOff() noexcept
{
    for (int p = 0; p < kPorts; ++p)
        allNotesOff(p);
}

void MidiOutScheduler::trackNotesOff(int track) noexcept
{
    for (int i = 0; i < numOffs_;)
    {
        const PendingOff o = offs_[static_cast<std::size_t>(i)];
        if (o.owner == track)
        {
            offs_[static_cast<std::size_t>(i)] = offs_[static_cast<std::size_t>(--numOffs_)];
            message(o.port, static_cast<std::uint8_t>(0x80 | o.ch), o.note, 0, 3, blockStart_);
            setSounding(o.port, o.ch, o.note, false);
        }
        else
            ++i;
    }
}

int MidiOutScheduler::soundingCount() const noexcept
{
    int n = 0;
    for (const auto& words : sounding_)
        for (auto w : words)
            n += __builtin_popcount(w);
    return n;
}

void MidiOutScheduler::endBlock() noexcept
{
    const std::int64_t end = blockStart_ + kBlockFrames;
    for (int i = 0; i < numOffs_;)
    {
        const PendingOff o = offs_[static_cast<std::size_t>(i)];
        if (o.at < end)
        {
            offs_[static_cast<std::size_t>(i)] = offs_[static_cast<std::size_t>(--numOffs_)];
            message(o.port, static_cast<std::uint8_t>(0x80 | o.ch), o.note, 0, 3, o.at);
            setSounding(o.port, o.ch, o.note, false);
        }
        else
            ++i;
    }
    // Stable insertion sort by time (a block holds few messages; already ordered input costs n compares).
    const auto later = [](const core::midi::MidiOutEvent& a, const core::midi::MidiOutEvent& b)
    { return a.sampleTime > b.sampleTime || (a.sampleTime == b.sampleTime && a.frac > b.frac); };
    for (int i = 1; i < numEvents_; ++i)
    {
        const auto e = events_[static_cast<std::size_t>(i)];
        int j = i;
        while (j > 0 && later(events_[static_cast<std::size_t>(j - 1)], e))
        {
            events_[static_cast<std::size_t>(j)] = events_[static_cast<std::size_t>(j - 1)];
            --j;
        }
        events_[static_cast<std::size_t>(j)] = e;
    }
    for (int i = 0; i < numEvents_; ++i)
    {
        const auto& e = events_[static_cast<std::size_t>(i)];
        if (hub_ != nullptr && (e.system() ? hub_->pushSync(e) : hub_->pushOut(e)))
            ++sent_;
        else
            ++dropped_;
    }
    numEvents_ = 0;
}
} // namespace engine::midi
