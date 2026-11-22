#include "io/controllers/profiles/LampEncoder.h"

namespace io::controllers
{
LampEncoder::LampEncoder(const LoadedProfile& p) : palettes_(p.palettes)
{
    index_.fill(-1);
    lamps_.reserve(p.lamps.size());
    for (const auto& l : p.lamps)
    {
        if (l.palette >= palettes_.size() || lampSlot(l.control) >= kLampSlots)
            continue;
        const int ch = (l.channel != 0 ? l.channel : (p.channel != 0 ? p.channel : 1)) - 1;
        Lamp lamp {{static_cast<std::uint8_t>((l.status == status::cc ? 0xB0 : 0x90) | ch), l.number},
                    &palettes_[l.palette]};
        lamp.want = static_cast<std::uint8_t>(palettes_[l.palette].front() >> 24); // off until the model says otherwise
        index_[static_cast<std::size_t>(lampSlot(l.control))] = static_cast<std::int16_t>(lamps_.size());
        lamps_.push_back(lamp);
    }
}

int LampEncoder::nearest(std::span<const std::uint32_t> palette, std::uint32_t rgb) noexcept
{
    const int r = static_cast<int>(rgb >> 16 & 255u), g = static_cast<int>(rgb >> 8 & 255u), b = static_cast<int>(rgb & 255u);
    int best = 0;
    long bestD = -1;
    for (std::size_t i = 0; i < palette.size(); ++i)
    {
        const int dr = r - static_cast<int>(palette[i] >> 16 & 255u), dg = g - static_cast<int>(palette[i] >> 8 & 255u),
                  db = b - static_cast<int>(palette[i] & 255u);
        const long d = 2L * dr * dr + 4L * dg * dg + 3L * db * db; // rough perceptual weights
        if (bestD < 0 || d < bestD)
        {
            bestD = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

void LampEncoder::set(Control c, std::uint32_t rgb, std::uint8_t level) noexcept
{
    const int slot = lampSlot(c);
    if (slot < 0 || slot >= kLampSlots || index_[static_cast<std::size_t>(slot)] < 0)
        return;
    auto& lamp = lamps_[static_cast<std::size_t>(index_[static_cast<std::size_t>(slot)])];
    const auto scale = [level](std::uint32_t ch) { return (ch * level + 127u) / 255u; };
    const std::uint32_t shown = level == 0 ? 0u
                                           : scale(rgb >> 16 & 255u) << 16 | scale(rgb >> 8 & 255u) << 8 | scale(rgb & 255u);
    const auto& pal = *lamp.palette;
    lamp.want = static_cast<std::uint8_t>(pal[static_cast<std::size_t>(nearest(pal, shown))] >> 24);
}

int LampEncoder::flush(double nowMs, const Send& send)
{
    if (lamps_.empty() || nowMs - lastFlushMs_ < kMinFlushMs)
        return 0;
    int sent = 0;
    for (auto& lamp : lamps_)
    {
        if (lamp.sent == lamp.want)
            continue;
        const std::uint8_t msg[3] = {lamp.head[0], lamp.head[1], lamp.want};
        if (send && send(msg, 3))
        {
            lamp.sent = lamp.want;
            ++sent;
        }
    }
    if (sent > 0)
        lastFlushMs_ = nowMs;
    return sent;
}

void LampEncoder::resend() noexcept
{
    for (auto& lamp : lamps_)
        lamp.sent = -1;
}

int LampEncoder::value(Control c) const noexcept
{
    const int slot = lampSlot(c);
    if (slot < 0 || slot >= kLampSlots || index_[static_cast<std::size_t>(slot)] < 0)
        return -1;
    return lamps_[static_cast<std::size_t>(index_[static_cast<std::size_t>(slot)])].want;
}
} // namespace io::controllers
