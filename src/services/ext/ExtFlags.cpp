#include "services/ext/ExtFlags.h"

#include "ProjectIdentity.h"

#include <cctype>
#include <cstdlib>

namespace services::ext
{
std::optional<float> Flags::value(std::string_view key) const
{
    if (auto it = overrides_.find(key); it != overrides_.end())
        return it->second;
    if (source_)
        return source_(key);
    return std::nullopt;
}

bool Flags::isOn(std::string_view flag) const
{
    if (flag.empty())
        return true;
    auto on = [this](std::string_view key) {
        const auto v = value(key);
        return v.has_value() && *v >= 0.5f;
    };
    if (! on(kMaster))
        return false;
    if (flag == kMaster)
        return true;
    if (flag.substr(0, 4) == "ext.")
        return on(flag);
    return on("ext." + std::string(flag));
}

void Flags::set(std::string_view key, bool on)
{
    std::string k(key);
    if (k.rfind("ext.", 0) != 0)
        k = "ext." + k;
    overrides_[k] = on ? 1.0f : 0.0f;
}

std::optional<float> environmentValue(std::string_view key)
{
    std::string name = identity::envPrefix;
    name += '_';
    for (const char c : key)
        name += c == '.' ? '_' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const char* v = std::getenv(name.c_str());
    if (v == nullptr || *v == '\0')
        return std::nullopt;
    const std::string s(v);
    if (s == "on" || s == "true" || s == "yes")
        return 1.0f;
    if (s == "off" || s == "false" || s == "no")
        return 0.0f;
    return static_cast<float>(std::atof(v));
}

Flags& flags()
{
    static Flags f = [] {
        Flags x;
        x.setSource(&environmentValue);
        return x;
    }();
    return f;
}
} // namespace services::ext
