#include "ui/input/pads/PadLamps.h"

namespace ui::input::pads
{
namespace
{
constexpr io::Control kModeKeys[] = {io::Control::PadModeTrack, io::Control::PadModeMute, io::Control::PadModeSlice,
                                     io::Control::PadModeScale};

io::LampState stateOf(std::uint32_t rgb, std::uint8_t level) noexcept
{
    if (level == 0 || (rgb & 0xFFFFFFu) == 0)
        return {};
    return {level >= 200 ? io::LampMode::On : level > io::kFaintLevel ? io::LampMode::Dim : io::LampMode::Faint,
            rgb & 0xFFFFFFu, 0};
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

void PadLamps::overlay(io::LampFrame& f, PadMode mode) const noexcept
{
    if (! painted_)
        return;
    for (std::size_t i = 0; i < 4; ++i)
        f[kModeKeys[i]] = keys_[i];
    if (mode == PadMode::Play)
        return;
    for (int p = 1; p <= 16; ++p)
        f[io::pad(p)] = pads_[static_cast<std::size_t>(p - 1)];
}
} // namespace ui::input::pads
