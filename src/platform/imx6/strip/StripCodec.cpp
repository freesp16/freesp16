// Touch strip CDC frames (see StripCodec.h). Device-independent: tests/unit/panel compiles it on the host.
#include "platform/imx6/strip/StripCodec.h"

namespace platform::imx6::strip
{
bool StripDecoder::decode(const std::uint8_t* frame, double t, io::ControlEvent& out) noexcept
{
    using Kind = io::ControlEvent::Kind;
    const auto s = decodeFrame(frame);
    if (s.contact && ! contact_)
    {
        contact_ = true;
        position_ = s.position;
        out = {Kind::Touch, io::Control::Strip, static_cast<std::int16_t>(s.position), io::kSourceStrip, t};
        return true;
    }
    if (s.contact && s.position != position_)
    {
        position_ = s.position;
        out = {Kind::Value, io::Control::Strip, static_cast<std::int16_t>(s.position), io::kSourceStrip, t};
        return true;
    }
    if (! s.contact && contact_)
    {
        contact_ = false;
        out = {Kind::Release, io::Control::Strip, static_cast<std::int16_t>(position_ < 0 ? 0 : position_),
               io::kSourceStrip, t};
        return true;
    }
    return false;
}
} // namespace platform::imx6::strip
