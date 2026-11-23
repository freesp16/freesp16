#include "ui/input/pads/PadLamps.h"

namespace ui::input::pads
{
namespace
{
constexpr io::Control kModeKeys[] = {io::Control::PadModeTrack, io::Control::PadModeMute, io::Control::PadModeSlice,
                                     io::Control::PadModeScale};

// The resolved level kept exactly (a flash mid-fade, a soloed pad mid-breath): the brightness preference scales it
// once more when the frame is applied; the faint level stays Faint, which never rounds down to dark.
io::LampState stateOf(std::uint32_t rgb, std::uint8_t level) noexcept
{
    if (level == 0 || (rgb & 0xFFFFFFu) == 0)
        return {};
    if (level <= io::kFaintLevel)
        return {io::LampMode::Faint, rgb & 0xFFFFFFu, 0};
    if (level == 255)
        return {io::LampMode::On, rgb & 0xFFFFFFu, 0};
    return {io::LampMode::Level, rgb & 0xFFFFFFu, 0, level};
}
} // namespace

void PadLamps::setLamp(io::Control c, std::uint32_t rgb, std::uint8_t level)
{
    if (io::isPad(c))
    {
        pads_[static_cast<std::size_t>((io::indexOf(c) - 1) & 15)] = stateOf(rgb, level);
        return;
    }
    for (std::size_t i = 0; i < 4; ++i)
        if (c == kModeKeys[i])
            keys_[i] = stateOf(rgb, level);
}

void PadLamps::overlay(io::LampFrame& f) const noexcept
{
    if (! painted_)
        return;
    for (std::size_t i = 0; i < 4; ++i)
        f[kModeKeys[i]] = keys_[i];
    // every pad mode, PLAY too: one painter, one rule (io::padLamp)
    for (int p = 1; p <= 16; ++p)
        f[io::pad(p)] = pads_[static_cast<std::size_t>(p - 1)];
}
} // namespace ui::input::pads
