#include "app/SettingsFile.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

extern char** environ; // POSIX: the process environment

namespace app
{
namespace
{
std::string_view trim(std::string_view s)
{
    while (! s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (! s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

std::string quoted(const std::string& v)
{
    if (! v.empty() && v.find_first_of(" \t\"") == std::string::npos)
        return v;
    std::string out = "\"";
    for (char c : v)
        out += c == '"' ? std::string("\\\"") : std::string(1, c);
    return out + "\"";
}
} // namespace

std::string normaliseSettingsKey(std::string_view raw)
{
    const std::string_view t = trim(raw);
    std::string key;
    key.reserve(t.size());
    for (std::size_t i = 0; i < t.size(); ++i)
    {
        if (t[i] == '_' && i + 1 < t.size() && t[i + 1] == '_')
        {
            key += '.';
            ++i;
            continue;
        }
        key += static_cast<char>(std::tolower(static_cast<unsigned char>(t[i])));
    }
    return key;
}

int applySettingsText(Settings& s, std::string_view text)
{
    int n = 0;
    std::size_t pos = 0;
    while (pos <= text.size())
    {
        std::size_t end = text.find('\n', pos);
        if (end == std::string_view::npos)
            end = text.size();
        const std::string_view line = trim(text.substr(pos, end - pos));
        pos = end + 1;
        if (line.empty() || line.front() == '#')
            continue;
        const auto eq = line.find('=');
        if (eq == std::string_view::npos)
            continue;
        const std::string key = normaliseSettingsKey(line.substr(0, eq));
        if (key.empty())
            continue;
        s[key] = std::string(trim(line.substr(eq + 1)));
        ++n;
    }
    return n;
}

int applySettingsEnvironment(Settings& s, std::string_view prefix, const char* const* envp)
{
    if (envp == nullptr)
        return 0;
    int n = 0;
    const std::string lead = std::string(prefix) + "_";
    for (const char* const* e = envp; *e != nullptr; ++e)
    {
        const std::string_view var(*e);
        if (var.size() <= lead.size() || var.compare(0, lead.size(), lead) != 0)
            continue;
        const auto eq = var.find('=');
        if (eq == std::string_view::npos || eq <= lead.size())
            continue;
        const std::string key = normaliseSettingsKey(var.substr(lead.size(), eq - lead.size()));
        if (key.empty())
            continue;
        s[key] = std::string(var.substr(eq + 1)); // verbatim: the shell already stripped what it wanted
        ++n;
    }
    return n;
}

Settings loadSettings(const Settings& defaults, const std::string& file, std::string_view prefix,
                      const char* const* envp, SettingsOrigin* origin)
{
    Settings s = defaults;
    SettingsOrigin o;
    o.file = file;
    if (! file.empty())
        if (std::ifstream in(file); in)
        {
            std::stringstream text;
            text << in.rdbuf();
            o.fileFound = true;
            o.fileKeys = applySettingsText(s, text.str());
        }
    o.envKeys = applySettingsEnvironment(s, prefix, envp != nullptr ? envp : environ);
    if (origin != nullptr)
        *origin = o;
    return s;
}

std::string settingsEnvironmentName(std::string_view prefix, std::string_view key)
{
    std::string name = std::string(prefix) + "_";
    for (char c : key)
        name += c == '.' ? std::string("__") : std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    return name;
}

int exportSettingsToEnvironment(const Settings& effective, const Settings& defaults, std::string_view prefix)
{
    int n = 0;
    for (const auto& [key, value] : effective)
    {
        const auto d = defaults.find(key);
        if (d != defaults.end() && d->second == value)
            continue;
        const std::string name = settingsEnvironmentName(prefix, key);
        if (std::getenv(name.c_str()) != nullptr)
            continue;
        if (::setenv(name.c_str(), value.c_str(), 0) == 0)
            ++n;
    }
    return n;
}

std::string describeSettings(const Settings& effective, const Settings& defaults, const SettingsOrigin& origin)
{
    std::string out = "file=" + quoted(origin.file.empty() ? std::string("(none)") : origin.file)
                    + (origin.fileFound ? " (read)" : " (missing)") + " file_keys=" + std::to_string(origin.fileKeys)
                    + " env_keys=" + std::to_string(origin.envKeys) + " |";
    int shown = 0;
    for (const auto& [key, value] : effective) // std::map: sorted
    {
        const auto d = defaults.find(key);
        if (d != defaults.end() && d->second == value)
            continue;
        out += " " + key + "=" + quoted(value);
        ++shown;
    }
    if (shown == 0)
        out += " (defaults)";
    return out;
}
} // namespace app
