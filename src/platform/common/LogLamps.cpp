#include "platform/common/LogLamps.h"

#include "ProjectIdentity.h"

#include <chrono>
#include <cstdio>
#include <string>

namespace platform::common
{
void LogLamps::setLamp(io::Control c, std::uint32_t rgb, std::uint8_t level)
{
    const auto i = static_cast<std::size_t>(c);
    if (i < kCount)
    {
        rgb_[i] = rgb & 0xFFFFFFu;
        level_[i] = level;
    }
}

void LogLamps::flush()
{
    using namespace std::chrono;
    const double now = duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
    if (now - lastMs_ < 100.0)
        return;
    std::string line;
    for (std::size_t i = 0; i < kCount; ++i)
        if (rgb_[i] != shownRgb_[i] || level_[i] != shownLevel_[i])
        {
            char buf[64];
            const auto name = io::lampName(static_cast<io::Control>(i));
            std::snprintf(buf, sizeof buf, " %.*s=%06x/%u", static_cast<int>(name.size()), name.data(),
                          static_cast<unsigned>(rgb_[i]), static_cast<unsigned>(level_[i]));
            line += buf;
            shownRgb_[i] = rgb_[i];
            shownLevel_[i] = level_[i];
        }
    if (line.empty())
        return;
    lastMs_ = now;
    std::printf("[sp16-mod] %s LAMP%s\n", identity::envPrefix, line.c_str());
    std::fflush(stdout);
}
} // namespace platform::common
