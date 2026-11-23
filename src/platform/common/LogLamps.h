// Logged lamps (a stand-in for physical lamps beside the on-screen virtual panel): with the setting
// lamp_log=1 a LampSink that prints one `LAMP <control>=<rrggbb>/<level> ...` line per flush in which lamps changed,
// at most every 100 ms (changes in between are merged). The linux platform uses it alone (else no lamps); imx6
// puts it beside the panel's own lamps (TeeLamps) so a hardware run can compare the log with the panel.
#pragma once

#include "io/controllers/ControlNames.h"
#include "io/controllers/ControlSurface.h"

#include <array>
#include <cstdint>
#include <memory>

namespace platform::common
{
class LogLamps final : public io::LampSink
{
public:
    void setLamp(io::Control, std::uint32_t rgb, std::uint8_t level) override;
    void flush() override;

private:
    static constexpr std::size_t kCount = static_cast<std::size_t>(io::kLampSlots); // + the bar position lamps
    std::array<std::uint32_t, kCount> rgb_ {}, shownRgb_ {};
    std::array<std::uint8_t, kCount> level_ {}, shownLevel_ {};
    double lastMs_ = -1e9;
};

// Both sinks get every call (the first one first).
class TeeLamps final : public io::LampSink
{
public:
    TeeLamps(std::unique_ptr<io::LampSink> first, std::unique_ptr<io::LampSink> second)
        : first_(std::move(first)), second_(std::move(second))
    {
    }
    void setLamp(io::Control c, std::uint32_t rgb, std::uint8_t level) override
    {
        first_->setLamp(c, rgb, level);
        second_->setLamp(c, rgb, level);
    }
    void setAnalogFilter(bool inUse, bool master, int hpCutoff) override
    {
        first_->setAnalogFilter(inUse, master, hpCutoff);
        second_->setAnalogFilter(inUse, master, hpCutoff);
    }
    void flush() override
    {
        first_->flush();
        second_->flush();
    }

private:
    std::unique_ptr<io::LampSink> first_, second_;
};
} // namespace platform::common
