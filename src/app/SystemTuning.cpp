#include "app/SystemTuning.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#if defined(__linux__)
 #include <sys/mman.h>
 #include <unistd.h>
#endif

namespace app::tuning
{
namespace
{
std::string firstLine(const std::string& path)
{
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    return line;
}

bool writeLine(const std::string& path, const std::string& value)
{
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (f == nullptr)
        return false;
    const bool ok = std::fputs(value.c_str(), f) >= 0;
    return std::fclose(f) == 0 && ok;
}

long swapTotalKb()
{
    std::ifstream in("/proc/meminfo");
    for (std::string line; std::getline(in, line);)
        if (line.rfind("SwapTotal:", 0) == 0)
            return std::strtol(line.c_str() + 10, nullptr, 10);
    return -1;
}
} // namespace

CodeLock lockCode()
{
    CodeLock r;
#if defined(__linux__)
    r.swapKb = swapTotalKb();
    char exe[4096] = {};
    const auto n = ::readlink("/proc/self/exe", exe, sizeof exe - 1);
    const std::string self = n > 0 ? std::string(exe, static_cast<std::size_t>(n)) : std::string();
    std::ifstream maps("/proc/self/maps");
    for (std::string line; std::getline(maps, line);)
    {
        // start-end perms offset dev inode path
        unsigned long start = 0, end = 0;
        char perms[8] = {};
        int pathAt = 0;
        if (std::sscanf(line.c_str(), "%lx-%lx %7s %*s %*s %*s %n", &start, &end, perms, &pathAt) < 3 || pathAt <= 0)
            continue;
        std::string path = line.substr(static_cast<std::size_t>(pathAt));
        if (path.empty() || path[0] != '/') // anonymous, [stack], [heap], [vdso], [vectors]
            continue;
        if (const auto deleted = path.find(" (deleted)"); deleted != std::string::npos)
            path.resize(deleted);
        const bool readable = perms[0] == 'r' || perms[2] == 'x';
        if (! readable || (perms[2] != 'x' && path != self))
            continue;
        const long kb = static_cast<long>((end - start) / 1024);
        if (::mlock(reinterpret_cast<void*>(start), end - start) == 0)
        {
            ++r.mappings;
            r.lockedKb += kb;
        }
        else
        {
            if (r.failed++ == 0)
                r.firstError = errno;
            r.failedKb += kb;
        }
    }
#endif
    return r;
}

std::string describe(const CodeLock& r)
{
    std::ostringstream s;
    s << "memlock code locked_kb=" << r.lockedKb << " mappings=" << r.mappings;
    if (r.failed > 0)
        s << " failed=" << r.failed << " failed_kb=" << r.failedKb << " (" << std::strerror(r.firstError) << ")";
    s << " swap_kb=" << (r.swapKb < 0 ? std::string("n/a") : std::to_string(r.swapKb));
    return s.str();
}

Governor setGovernor(const std::string& name)
{
    Governor g;
    g.before = firstLine("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
    if (g.before.empty())
    {
        g.note = "no cpufreq";
        return g;
    }
    if (name.empty())
        return g;
    for (int cpu = 0; cpu < 32; ++cpu)
    {
        const std::string dir = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpufreq/";
        const std::string current = firstLine(dir + "scaling_governor");
        if (current.empty())
            continue; // offline, absent, or sharing cpu0's policy without its own files
        if (current == name)
            continue;
        const std::string offered = " " + firstLine(dir + "scaling_available_governors") + " ";
        if (offered.find(" " + name + " ") == std::string::npos)
        {
            g.note = name + " not offered (" + offered.substr(1, offered.size() - 2) + ")";
            ++g.failed;
            continue;
        }
        if (writeLine(dir + "scaling_governor", name))
        {
            g.previous.emplace_back(dir + "scaling_governor", current);
            ++g.written;
            g.changed = true;
        }
        else
        {
            if (g.note.empty())
                g.note = std::string("cannot write ") + dir + "scaling_governor: " + std::strerror(errno);
            ++g.failed;
        }
    }
    return g;
}

namespace
{
constexpr const char* kRtRuntime = "/proc/sys/kernel/sched_rt_runtime_us";
}

RtRuntime setRtRuntime(const std::string& value)
{
    RtRuntime r;
    r.before = firstLine(kRtRuntime);
    if (r.before.empty())
    {
        r.note = "not readable";
        return r;
    }
    if (value.empty() || value == r.before)
        return r;
    if (writeLine(kRtRuntime, value))
        r.changed = true;
    else
        r.note = std::string("cannot write: ") + std::strerror(errno);
    return r;
}

void restore(const RtRuntime& r)
{
    if (r.changed)
        (void) writeLine(kRtRuntime, r.before);
}

std::string describe(const RtRuntime& r, const std::string& wanted)
{
    std::string s = "rt_runtime_us before=" + (r.before.empty() ? std::string("n/a") : r.before)
                  + " wanted=" + (wanted.empty() ? std::string("keep") : wanted) + " now=" + firstLine(kRtRuntime);
    if (! r.note.empty())
        s += " (" + r.note + ")";
    return s;
}

void restore(const Governor& g)
{
    for (auto it = g.previous.rbegin(); it != g.previous.rend(); ++it)
        (void) writeLine(it->first, it->second);
}

std::string describe(const Governor& g, const std::string& wanted)
{
    std::string s = "governor before=" + (g.before.empty() ? std::string("n/a") : g.before) + " wanted="
                  + (wanted.empty() ? std::string("keep") : wanted)
                  + " now=" + firstLine("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor")
                  + " written=" + std::to_string(g.written) + " failed=" + std::to_string(g.failed);
    if (! g.note.empty())
        s += " (" + g.note + ")";
    return s;
}
} // namespace app::tuning
