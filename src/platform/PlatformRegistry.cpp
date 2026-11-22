// platform::select: "auto" returns the first registered platform whose detect() is true (host last),
// else the host platform, else — in a build that carries only device platforms — the first one built in. A named
// platform (setting `platform`, environment <PREFIX>_PLATFORM) is used when it is built in, else "auto" applies.
#include "platform/Platform.h"

#include <algorithm>
#include <utility>

namespace platform
{
namespace
{
std::vector<std::pair<std::string_view, PlatformFactory>>& entries()
{
    static std::vector<std::pair<std::string_view, PlatformFactory>> list;
    return list;
}

PlatformFactory find(std::string_view id)
{
    for (const auto& [name, factory] : entries())
        if (name == id)
            return factory;
    return nullptr;
}
} // namespace

void registerPlatform(std::string_view id, PlatformFactory factory)
{
    if (find(id) == nullptr && factory != nullptr)
        entries().emplace_back(id, factory);
}

std::vector<std::string_view> registeredPlatforms()
{
    std::vector<std::string_view> ids;
    for (const auto& e : entries())
        ids.push_back(e.first);
    std::sort(ids.begin(), ids.end());
    return ids;
}

std::unique_ptr<Platform> select(std::string_view wanted, std::string& log)
{
    std::string builtIn;
    for (auto id : registeredPlatforms())
        builtIn += (builtIn.empty() ? "" : ",") + std::string(id);
    if (wanted != "auto" && ! wanted.empty())
    {
        if (auto factory = find(wanted))
        {
            log = std::string(wanted) + " selected (requested; built in: " + builtIn + ")";
            return factory();
        }
        log = "requested platform '" + std::string(wanted) + "' is not built in; ";
    }
    for (auto id : registeredPlatforms())
    {
        if (id == "host")
            continue; // the host platform is the fallback, never "detected" ahead of real hardware
        auto p = find(id)();
        if (p->detect())
        {
            log += std::string(id) + " selected (auto: detected; built in: " + builtIn + ")";
            return p;
        }
    }
    if (auto factory = find("host"))
    {
        log += "host selected (auto: no hardware detected; built in: " + builtIn + ")";
        return factory();
    }
    const auto ids = registeredPlatforms();
    if (ids.empty())
    {
        log += "no platform built in";
        return nullptr;
    }
    log += std::string(ids.front()) + " selected (auto: nothing detected, only device platforms built in: " + builtIn + ")";
    return find(ids.front())();
}
} // namespace platform
