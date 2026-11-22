#include "app/Diagnostics.h"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#include <ucontext.h>
#endif
#if defined(__GLIBC__)
#include <execinfo.h>
#include <gnu/libc-version.h>
#define APP_DIAG_BACKTRACE 1
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace app::diag
{
namespace
{
std::string readFirstLine(const char* path)
{
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    return line;
}

std::string mb(std::int64_t bytes)
{
    if (bytes < 0)
        return "n/a";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buf;
}

std::string statusField(const char* field)
{
    std::ifstream in("/proc/self/status");
    const std::size_t n = std::strlen(field);
    for (std::string line; std::getline(in, line);)
        if (line.compare(0, n, field) == 0)
            return line.substr(n);
    return {};
}

struct Mount
{
    std::string device, point, type;
};

std::vector<Mount> mounts()
{
    std::vector<Mount> out;
    std::ifstream in("/proc/mounts");
    for (std::string line; std::getline(in, line);)
    {
        std::istringstream f(line);
        Mount m;
        if (! (f >> m.device >> m.point >> m.type))
            continue;
        std::string decoded; // /proc/mounts escapes blanks as \040
        for (std::size_t i = 0; i < m.point.size(); ++i)
            if (m.point.compare(i, 4, "\\040") == 0)
                decoded += ' ', i += 3;
            else
                decoded += m.point[i];
        m.point = decoded;
        out.push_back(m);
    }
    return out;
}

std::string cpuinfoValue(const std::string& text, const char* key)
{
    std::istringstream in(text);
    const std::size_t n = std::strlen(key);
    for (std::string line; std::getline(in, line);)
        if (line.compare(0, n, key) == 0)
        {
            const auto colon = line.find(':');
            if (colon == std::string::npos)
                continue;
            std::string v = line.substr(colon + 1);
            while (! v.empty() && v.front() == ' ')
                v.erase(v.begin());
            return v;
        }
    return {};
}

std::vector<std::string> listDir(const char* path, const char* prefix)
{
    std::vector<std::string> out;
    if (DIR* d = ::opendir(path))
    {
        while (const dirent* e = ::readdir(d))
            if (std::strncmp(e->d_name, prefix, std::strlen(prefix)) == 0)
                out.emplace_back(e->d_name);
        ::closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ---- crash handler (async-signal-safe helpers: no allocation, no stdio) ----
char gTag[64] = "CRASH";
alignas(16) char gAltStack[64 * 1024];

void put(const char* s) noexcept
{
    std::size_t n = 0;
    while (s[n] != 0)
        ++n;
    while (n > 0)
    {
        const ssize_t w = ::write(STDOUT_FILENO, s, n);
        if (w <= 0)
            return;
        s += w;
        n -= static_cast<std::size_t>(w);
    }
}

void putHex(std::uintptr_t v) noexcept
{
    char buf[2 + 2 * sizeof v + 1];
    buf[0] = '0';
    buf[1] = 'x';
    for (std::size_t i = 0; i < 2 * sizeof v; ++i)
        buf[2 + i] = "0123456789abcdef"[(v >> (4 * (2 * sizeof v - 1 - i))) & 15];
    buf[sizeof buf - 1] = 0;
    put(buf);
}

void putDec(long v) noexcept
{
    char buf[24];
    int i = 23;
    buf[i] = 0;
    const bool neg = v < 0;
    unsigned long u = neg ? static_cast<unsigned long>(-(v + 1)) + 1 : static_cast<unsigned long>(v);
    do
    {
        buf[--i] = static_cast<char>('0' + u % 10);
        u /= 10;
    } while (u != 0 && i > 1);
    if (neg)
        buf[--i] = '-';
    put(buf + i);
}

const char* signalName(int sig) noexcept
{
    switch (sig)
    {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    case SIGABRT: return "SIGABRT";
    default: return "signal";
    }
}

// The r-xp lines of /proc/self/maps (where the program and its libraries are: a backtrace address minus the start of
// its mapping + the offset is what addr2line wants for a library; the non-PIE program is at its link address).
void putExecutableMaps() noexcept
{
    const int fd = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    char buf[4096];
    char line[512];
    std::size_t len = 0;
    for (;;)
    {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n <= 0)
            break;
        for (ssize_t i = 0; i < n; ++i)
        {
            if (buf[i] != '\n')
            {
                if (len + 1 < sizeof line)
                    line[len++] = buf[i];
                continue;
            }
            line[len] = 0;
            // "start-end perms offset dev inode path": keep executable file mappings only
            const char* perms = nullptr;
            for (std::size_t k = 0; k + 1 < len; ++k)
                if (line[k] == ' ')
                {
                    perms = line + k + 1;
                    break;
                }
            if (perms != nullptr && perms[2] == 'x' && std::strchr(line, '/') != nullptr)
            {
                put(gTag);
                put(" CRASH map ");
                put(line);
                put("\n");
            }
            len = 0;
        }
    }
    ::close(fd);
}

void onFatalSignal(int sig, siginfo_t* info, void* context) noexcept
{
    put(gTag);
    put(" CRASH ");
    put(signalName(sig));
    put(" (");
    putDec(sig);
    put(") code=");
    putDec(info != nullptr ? info->si_code : 0);
    put(" addr=");
    putHex(info != nullptr ? reinterpret_cast<std::uintptr_t>(info->si_addr) : 0);
#if defined(__linux__) && defined(__arm__)
    if (context != nullptr)
    {
        const auto* uc = static_cast<const ucontext_t*>(context);
        put(" pc=");
        putHex(uc->uc_mcontext.arm_pc);
        put(" lr=");
        putHex(uc->uc_mcontext.arm_lr);
        put(" sp=");
        putHex(uc->uc_mcontext.arm_sp);
    }
#elif defined(__linux__) && defined(__x86_64__)
    if (context != nullptr)
    {
        const auto* uc = static_cast<const ucontext_t*>(context);
        put(" pc=");
        putHex(static_cast<std::uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]));
        put(" sp=");
        putHex(static_cast<std::uintptr_t>(uc->uc_mcontext.gregs[REG_RSP]));
    }
#else
    (void)context;
#endif
#if defined(__linux__)
    put(" tid=");
    putDec(static_cast<long>(::syscall(SYS_gettid)));
#endif
    put(" pid=");
    putDec(static_cast<long>(::getpid()));
    put("\n");
#if defined(APP_DIAG_BACKTRACE)
    void* frames[48];
    const int n = ::backtrace(frames, 48);
    put(gTag);
    put(" CRASH backtrace frames=");
    putDec(n);
    put(" (addresses: addr2line -C -f -e <unstripped binary> <addr>)\n");
    ::backtrace_symbols_fd(frames, n, STDOUT_FILENO);
#endif
    putExecutableMaps();
    put(gTag);
    put(" CRASH end\n");
    ::fsync(STDOUT_FILENO);
    ::signal(sig, SIG_DFL); // die with the default action: the exit status names the signal
    ::raise(sig);
}
} // namespace

std::int64_t freeBytes(const std::string& path)
{
    struct statvfs v {};
    if (::statvfs(path.c_str(), &v) != 0)
        return -1;
    return static_cast<std::int64_t>(v.f_bavail) * static_cast<std::int64_t>(v.f_frsize);
}

long residentKb()
{
    const std::string v = statusField("VmRSS:");
    return v.empty() ? -1 : std::atol(v.c_str());
}

long peakResidentKb()
{
    const std::string v = statusField("VmHWM:");
    return v.empty() ? -1 : std::atol(v.c_str());
}

std::vector<std::string> fatMounts()
{
    std::vector<std::string> out;
    for (const auto& m : mounts())
        if (m.type == "vfat" || m.type == "msdos" || m.type == "exfat" || m.type == "fuseblk")
            out.push_back(m.point);
    return out;
}

std::string mountOf(const std::string& path)
{
    const Mount* best = nullptr;
    const auto all = mounts();
    for (const auto& m : all)
    {
        const bool under = path == m.point || m.point == "/"
                        || (path.compare(0, m.point.size(), m.point) == 0 && path.size() > m.point.size()
                            && path[m.point.size()] == '/');
        if (under && (best == nullptr || m.point.size() >= best->point.size()))
            best = &m;
    }
    return best != nullptr ? best->point + " " + best->type : std::string();
}

void systemLines(const platform::Paths& paths, const Log& log)
{
    struct utsname u {};
    if (::uname(&u) == 0)
        log(std::string("kernel ") + u.sysname + " " + u.release + " " + u.machine + " (" + u.version + ") host="
            + u.nodename);
    std::string cpuinfo;
    {
        std::ifstream in("/proc/cpuinfo");
        std::stringstream text;
        text << in.rdbuf();
        cpuinfo = text.str();
    }
    std::string model = cpuinfoValue(cpuinfo, "model name");
    if (model.empty())
        model = cpuinfoValue(cpuinfo, "Processor");
    const std::string features = cpuinfoValue(cpuinfo, "Features").empty() ? cpuinfoValue(cpuinfo, "flags")
                                                                            : cpuinfoValue(cpuinfo, "Features");
    const bool neon = (" " + features + " ").find(" neon ") != std::string::npos;
    const std::string freq = readFirstLine("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq");
    const std::string maxFreq = readFirstLine("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
    const std::string governor = readFirstLine("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
    log("cpu online=" + std::to_string(::sysconf(_SC_NPROCESSORS_ONLN))
        + " configured=" + std::to_string(::sysconf(_SC_NPROCESSORS_CONF)) + " model=\"" + model + "\" hardware=\""
        + cpuinfoValue(cpuinfo, "Hardware") + "\" neon=" + (neon ? "yes" : "no") + " freq_khz="
        + (freq.empty() ? "n/a" : freq) + " max_khz=" + (maxFreq.empty() ? "n/a" : maxFreq)
        + " governor=" + (governor.empty() ? "n/a" : governor));
#if defined(__GLIBC__)
    log(std::string("libc glibc ") + gnu_get_libc_version() + " (" + gnu_get_libc_release() + "; built against "
        + std::to_string(__GLIBC__) + "." + std::to_string(__GLIBC_MINOR__) + ") compiler " + __VERSION__);
#else
    log(std::string("libc not glibc; compiler ") + __VERSION__);
#endif
    {
        std::ifstream in("/proc/meminfo");
        std::string text = "memory";
        for (std::string line; std::getline(in, line);)
            for (const char* key : {"MemTotal:", "MemFree:", "MemAvailable:", "Buffers:", "Cached:", "SwapTotal:"})
                if (line.compare(0, std::strlen(key), key) == 0)
                {
                    std::string k(key, std::strlen(key) - 1);
                    std::transform(k.begin(), k.end(), k.begin(), ::tolower);
                    text += " " + k + "=" + std::to_string(std::atol(line.c_str() + std::strlen(key))) + "kB";
                }
        log(text + " rss=" + std::to_string(residentKb()) + "kB");
    }
    for (const auto& [label, dir] : {std::pair<const char*, std::string> {"data", paths.dataDir},
                                     std::pair<const char*, std::string> {"cache", paths.cacheDir}})
    {
        const bool exists = ::access(dir.c_str(), F_OK) == 0;
        const bool writable = exists && ::access(dir.c_str(), W_OK) == 0;
        const std::string mount = mountOf(dir);
        log(std::string(label) + " dir=" + dir + (exists ? (writable ? " writable" : " READ-ONLY") : " missing")
            + " free=" + mb(freeBytes(exists ? dir : "/")) + " mount=" + (mount.empty() ? "n/a" : mount));
    }
    {
        std::string text;
        for (const auto& m : fatMounts())
            text += (text.empty() ? "" : ",") + m;
        log("storage fat_mounts=" + (text.empty() ? std::string("none") : text));
    }
    {
        std::ifstream in("/proc/asound/cards");
        int n = 0;
        for (std::string line; std::getline(in, line);)
            if (line.size() > 4 && line[0] == ' ' && line.find('[') != std::string::npos)
            {
                while (! line.empty() && line.front() == ' ')
                    line.erase(line.begin());
                log("sound card " + line);
                ++n;
            }
        if (n == 0)
            log("sound cards none (/proc/asound/cards unreadable or empty)");
    }
    {
        std::ifstream in("/proc/asound/pcm");
        for (std::string line; std::getline(in, line);)
            log("sound pcm " + line);
    }
    {
        std::string ports;
        for (const auto& name : listDir("/dev/snd", "midiC"))
            ports += (ports.empty() ? "" : ",") + name;
        std::string seq = ::access("/dev/snd/seq", F_OK) == 0 ? "present" : "absent";
        log("midi rawmidi=" + (ports.empty() ? std::string("none") : ports) + " seq=" + seq
            + " (names only; the MIDI sources log their own lines)");
    }
}

std::string describePorts(const platform::AudioDevice& device)
{
    static constexpr const char* kNames[] = {"MAIN", "OUT3-4", "OUT5-6", "OUT7-8", "PHONES", "FILTERSEND"};
    bool seen[std::size(kNames)] = {};
    std::string out;
    for (const auto& p : device.ports())
    {
        const auto i = static_cast<std::size_t>(p.port);
        if (i >= std::size(kNames))
            continue;
        seen[i] = true;
        out += std::string(out.empty() ? "" : " ") + kNames[i] + "="
             + (p.left < 0 || p.right < 0 ? std::string("absent")
                                          : std::to_string(p.left) + "/" + std::to_string(p.right));
    }
    for (std::size_t i = 0; i < std::size(kNames); ++i)
        if (! seen[i])
            out += std::string(out.empty() ? "" : " ") + kNames[i] + "=absent";
    return out;
}

void lineBufferStdout()
{
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
}

void installCrashHandler(const char* tag)
{
    std::snprintf(gTag, sizeof gTag, "%s", tag != nullptr ? tag : "");
#if defined(APP_DIAG_BACKTRACE)
    void* warm[4];
    (void)::backtrace(warm, 4); // loads the unwinder now: the first call may allocate, which a handler must not
#endif
    stack_t ss {};
    ss.ss_sp = gAltStack;
    ss.ss_size = sizeof gAltStack;
    ss.ss_flags = 0;
    ::sigaltstack(&ss, nullptr);
    struct sigaction sa {};
    sa.sa_sigaction = &onFatalSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT})
        ::sigaction(sig, &sa, nullptr);
}
} // namespace app::diag
