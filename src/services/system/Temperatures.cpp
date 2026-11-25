#include "services/system/Temperatures.h"

#include "services/library/Workers.h"

#include <cmath>
#include <dirent.h>
#include <fstream>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace services::system
{
namespace
{
bool component(std::string_view type, std::string_view name)
{
    if (! type.starts_with(name))
        return false;
    const auto suffix = type.substr(name.size());
    return suffix.empty() || suffix.front() == '-' || suffix.front() == '_' ||
           (suffix.front() >= '0' && suffix.front() <= '9');
}

struct Zone
{
    std::string path;
    bool wholeDegrees;
};

std::optional<int> hottest(const std::vector<Zone>& zones)
{
    std::optional<int> result;
    for (const auto& zone : zones)
    {
        // Legacy ANATOP reports whole degrees. Standard zones report millidegrees, even near zero.
        std::ifstream input(zone.path);
        long raw;
        char extra;
        if (! (input >> raw) || (input >> extra))
            continue;
        const double celsius = zone.wholeDegrees ? raw : raw / 1000.0;
        if (celsius < -60 || celsius > 200)
            continue;
        const int rounded = static_cast<int>(std::lround(celsius));
        if (! result || rounded > *result)
            result = rounded;
    }
    return result;
}
} // namespace

struct TemperatureMonitor::Sensors
{
    std::vector<Zone> cpu, gpu, soc;

    explicit Sensors(const std::string& root)
    {
        // Discover the zone types once at startup. Sampling the temperature itself is opt-in.
        std::unique_ptr<DIR, int (*)(DIR*)> dir(::opendir(root.c_str()), ::closedir);
        if (! dir)
            return;
        while (const auto* entry = ::readdir(dir.get()))
        {
            if (! std::string_view(entry->d_name).starts_with("thermal_zone"))
                continue;
            const auto path = root + "/" + entry->d_name;
            std::string type;
            std::ifstream(path + "/type") >> type;
            const Zone zone{path + "/temp", type == "anatoptz"};
            if (::access(zone.path.c_str(), R_OK) != 0)
                continue;
            if (type == "anatoptz" || type == "imx_thermal_zone")
                soc.push_back(zone);
            else if (component(type, "cpu") || type == "x86_pkg_temp")
                cpu.push_back(zone);
            else if (component(type, "gpu"))
                gpu.push_back(zone);
        }
    }

    Temperatures sample(TemperatureSelection wanted) const
    {
        return {wanted.cpu ? hottest(cpu) : std::nullopt, wanted.gpu ? hottest(gpu) : std::nullopt,
                wanted.soc ? hottest(soc) : std::nullopt};
    }
};

struct TemperatureMonitor::Request
{
    Temperatures result;
    bool done = false, discard = false; // only touched on the message thread
};

TemperatureMonitor::TemperatureMonitor(library::Workers& workers, std::string root)
    : workers_(workers), sensors_(std::make_shared<Sensors>(root))
{
}

TemperatureSelection TemperatureMonitor::available() const
{
    const bool cpu = ! sensors_->cpu.empty(), gpu = ! sensors_->gpu.empty();
    return {cpu, gpu, ! sensors_->soc.empty() && ! (cpu && gpu)};
}

Temperatures TemperatureMonitor::poll(TemperatureSelection wanted, double nowMs)
{
    if (wanted != wanted_)
    {
        wanted_ = wanted;
        latest_ = {};
        nextReadMs_ = nowMs;
        if (pending_)
            pending_->discard = true;
    }
    if (pending_ && pending_->done)
    {
        if (! pending_->discard)
            latest_ = pending_->result;
        pending_.reset();
    }
    if (wanted.any() && ! pending_ && nowMs >= nextReadMs_)
    {
        nextReadMs_ = nowMs + 2000;
        pending_ = std::make_shared<Request>();
        workers_.run([req = pending_, sensors = sensors_, wanted] { req->result = sensors->sample(wanted); },
                     [req = pending_] { req->done = true; });
    }
    return latest_;
}
} // namespace services::system
