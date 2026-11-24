#include "core/util/ThreadRole.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>

#if defined(__linux__)
 #include <pthread.h>
 #include <sched.h>
 #include <sys/resource.h>
 #include <sys/syscall.h>
 #include <unistd.h>
#endif

namespace core::threads
{
namespace
{
constexpr int kRoles = static_cast<int>(Role::Count);

constexpr std::array<const char*, kRoles> kNames {"audio",    "clock", "midi_in",  "panel", "midi_out",
                                                  "strip",    "recorder", "ui",    "worker"};
constexpr std::array<RoleSpec, kRoles> kDefaults {{{true, 70, 0},
                                                   {true, 80, 0},
                                                   {true, 75, 0},
                                                   {true, 72, 0},
                                                   {true, 65, 0},
                                                   {true, 60, 0},
                                                   {true, 40, 0},
                                                   {false, 0, 0},
                                                   {false, 10, 0}}};

// One word per role (apply() reads it from any thread): cores in bits 0…31, priority + 128 in bits 32…39, realtime
// in bit 40.
std::uint64_t pack(const RoleSpec& s) noexcept
{
    return static_cast<std::uint64_t>(s.cores) | static_cast<std::uint64_t>((s.priority + 128) & 0xFF) << 32
         | (s.realtime ? 1ull << 40 : 0ull);
}

RoleSpec unpack(std::uint64_t w) noexcept
{
    return {(w >> 40 & 1u) != 0, static_cast<int>(w >> 32 & 0xFF) - 128, static_cast<std::uint32_t>(w)};
}

// 0 = the role's default (a packed spec is never 0: its priority field is ≥ 108).
std::array<std::atomic<std::uint64_t>, kRoles> table {};

// The cores the system has (read on the message thread in reset(): sysconf may allocate).
std::atomic<int> systemCores {0};

int countCores() noexcept
{
#if defined(__linux__)
    const long n = ::sysconf(_SC_NPROCESSORS_CONF);
    return n < 1 ? 1 : (n > 32 ? 32 : static_cast<int>(n));
#else
    return 1;
#endif
}

int roleOf(const std::string& name) noexcept
{
    for (int i = 0; i < kRoles; ++i)
        if (name == kNames[static_cast<std::size_t>(i)])
            return i;
    return -1;
}

std::string trim(const std::string& s)
{
    const auto b = s.find_first_not_of(" \t");
    const auto e = s.find_last_not_of(" \t");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

bool parseInt(const std::string& s, int lo, int hi, int& out)
{
    if (s.empty())
        return false;
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (end == nullptr || *end != 0 || v < lo || v > hi)
        return false;
    out = static_cast<int>(v);
    return true;
}

// "*", "-" or "0+3": false when it does not parse.
bool parseCores(const std::string& s, std::uint32_t& out)
{
    if (s == "*")
    {
        out = kAllCores;
        return true;
    }
    if (s == "-")
    {
        out = 0;
        return true;
    }
    std::uint32_t mask = 0;
    std::size_t start = 0;
    while (start <= s.size())
    {
        const auto plus = s.find('+', start);
        const std::string part = s.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        int core = 0;
        if (! parseInt(part, 0, 31, core))
            return false;
        mask |= 1u << core;
        if (plus == std::string::npos)
            break;
        start = plus + 1;
    }
    out = mask;
    return mask != 0;
}

std::string describeCores(std::uint32_t cores)
{
    if (cores == 0)
        return "-";
    if (cores == kAllCores)
        return "*";
    std::string out;
    for (int c = 0; c < 32; ++c)
        if ((cores >> c & 1u) != 0)
            out += (out.empty() ? "" : "+") + std::to_string(c);
    return out;
}
} // namespace

const char* roleName(Role role) noexcept
{
    const int i = static_cast<int>(role);
    return i >= 0 && i < kRoles ? kNames[static_cast<std::size_t>(i)] : "?";
}

RoleSpec defaultSpec(Role role) noexcept
{
    const int i = static_cast<int>(role);
    return i >= 0 && i < kRoles ? kDefaults[static_cast<std::size_t>(i)] : RoleSpec {};
}

RoleSpec spec(Role role) noexcept
{
    const int i = static_cast<int>(role);
    if (i < 0 || i >= kRoles)
        return {};
    const auto w = table[static_cast<std::size_t>(i)].load(std::memory_order_acquire);
    return w == 0 ? kDefaults[static_cast<std::size_t>(i)] : unpack(w);
}

void reset() noexcept
{
    systemCores.store(countCores(), std::memory_order_release);
    for (auto& word : table)
        word.store(0, std::memory_order_release);
}

bool configure(const std::string& map, std::string& note)
{
    if (systemCores.load(std::memory_order_acquire) == 0)
        systemCores.store(countCores(), std::memory_order_release);
    bool ok = true;
    std::size_t start = 0;
    while (start < map.size())
    {
        auto comma = map.find(',', start);
        if (comma == std::string::npos)
            comma = map.size();
        const std::string entry = trim(map.substr(start, comma - start));
        start = comma + 1;
        if (entry.empty())
            continue;
        const auto colon = entry.find(':');
        const int role = colon == std::string::npos ? -1 : roleOf(trim(entry.substr(0, colon)));
        std::string cores = colon == std::string::npos ? std::string() : trim(entry.substr(colon + 1));
        std::string value;
        if (const auto at = cores.find('@'); at != std::string::npos)
        {
            value = trim(cores.substr(at + 1));
            cores = trim(cores.substr(0, at));
        }
        RoleSpec s = role >= 0 ? spec(static_cast<Role>(role)) : RoleSpec {};
        std::uint32_t mask = 0;
        int priority = s.priority;
        const bool good = role >= 0 && parseCores(cores, mask)
                       && (value.empty()
                           || (s.realtime ? parseInt(value, 1, 99, priority) : parseInt(value, -20, 19, priority)));
        if (! good)
        {
            note += (note.empty() ? "" : ", ") + entry;
            ok = false;
            continue;
        }
        s.cores = mask;
        s.priority = priority;
        table[static_cast<std::size_t>(role)].store(pack(s), std::memory_order_release);
    }
    return ok;
}

std::string describe()
{
    std::string out;
    for (int i = 0; i < kRoles; ++i)
    {
        const auto s = spec(static_cast<Role>(i));
        out += (out.empty() ? "" : " ") + std::string(kNames[static_cast<std::size_t>(i)]) + ":" + describeCores(s.cores)
             + (s.realtime ? "@" : "@nice") + std::to_string(s.priority);
    }
    return out;
}

Applied apply(Role role, const char* name) noexcept
{
    Applied out;
    const int i = static_cast<int>(role);
    if (i < 0 || i >= kRoles)
        return out;
    const RoleSpec s = spec(role);
#if defined(__linux__)
    if (name != nullptr && *name != 0)
    {
        char shortName[16];
        std::strncpy(shortName, name, sizeof shortName - 1);
        shortName[sizeof shortName - 1] = 0;
        (void) pthread_setname_np(pthread_self(), shortName);
    }
    if (s.cores != 0)
    {
        const int cores = systemCores.load(std::memory_order_acquire);
        cpu_set_t set;
        CPU_ZERO(&set);
        for (int c = 0; c < (cores > 0 ? cores : 32) && c < CPU_SETSIZE; ++c)
            if ((s.cores >> c & 1u) != 0)
                CPU_SET(c, &set);
        if (CPU_COUNT(&set) > 0)
            out.pinned = ::sched_setaffinity(0, sizeof set, &set) == 0; // 0 = the calling thread
    }
    if (s.realtime)
    {
        sched_param p {};
        p.sched_priority = s.priority;
        out.realtime = pthread_setschedparam(pthread_self(), SCHED_FIFO, &p) == 0; // best effort
    }
    else
    {
        int policy = 0;
        sched_param p {};
        if (pthread_getschedparam(pthread_self(), &policy, &p) == 0 && policy != SCHED_OTHER)
        {
            p.sched_priority = 0; // created by a real-time thread: back to the normal class first
            (void) pthread_setschedparam(pthread_self(), SCHED_OTHER, &p);
        }
        // Per thread on Linux: setpriority on the thread id. Raising the nice value needs no right; lowering it does.
        const auto tid = static_cast<id_t>(::syscall(SYS_gettid));
        out.niced = ::setpriority(PRIO_PROCESS, tid, s.priority) == 0;
    }
#else
    (void) name;
    (void) s;
#endif
    return out;
}
} // namespace core::threads
