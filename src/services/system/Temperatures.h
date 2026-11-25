#pragma once

#include <memory>
#include <optional>
#include <string>

namespace services::library
{
class Workers;
}

namespace services::system
{
struct TemperatureSelection
{
    bool cpu = false, gpu = false, soc = false;
    bool any() const { return cpu || gpu || soc; }
    bool operator==(const TemperatureSelection&) const = default;
};

struct Temperatures
{
    std::optional<int> cpu, gpu, soc; // whole °C, with a distinct slot for the shared die sensor
};

// Called on the message thread. Sysfs reads run on the existing library workers.
class TemperatureMonitor
{
public:
    explicit TemperatureMonitor(library::Workers&, std::string root = "/sys/class/thermal");
    TemperatureSelection available() const;
    Temperatures poll(TemperatureSelection, double nowMs);

private:
    struct Request;
    library::Workers& workers_;
    struct Sensors;
    std::shared_ptr<const Sensors> sensors_;
    TemperatureSelection wanted_;
    Temperatures latest_;
    std::shared_ptr<Request> pending_;
    double nextReadMs_ = 0;
};
} // namespace services::system
