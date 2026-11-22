#include "core/util/CpuUsage.h"

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace core
{
namespace
{
#if defined(__linux__)
// The kernel's aggregate counters: "cpu  user nice system idle iowait irq softirq steal guest guest_nice".
bool readCounters(std::uint64_t& busy, std::uint64_t& total) noexcept
{
    const int fd = ::open("/proc/stat", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    char buf[256]; // the first line is well under 256 bytes
    const ssize_t n = ::read(fd, buf, sizeof buf - 1);
    ::close(fd);
    if (n <= 0)
        return false;
    buf[n] = 0;
    return CpuUsage::parseProcStat(buf, busy, total);
}
#elif defined(__APPLE__)
bool readCounters(std::uint64_t& busy, std::uint64_t& total) noexcept
{
    static const mach_port_t host = mach_host_self(); // one send right for the process, not one per sample
    host_cpu_load_info_data_t info {};
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(host, HOST_CPU_LOAD_INFO, reinterpret_cast<host_info_t>(&info), &count) != KERN_SUCCESS)
        return false;
    busy = std::uint64_t {info.cpu_ticks[CPU_STATE_USER]} + info.cpu_ticks[CPU_STATE_NICE]
           + info.cpu_ticks[CPU_STATE_SYSTEM];
    total = busy + info.cpu_ticks[CPU_STATE_IDLE];
    return true;
}
#else
bool readCounters(std::uint64_t&, std::uint64_t&) noexcept
{
    return false;
}
#endif
} // namespace

bool CpuUsage::parseProcStat(const char* text, std::uint64_t& busy, std::uint64_t& total) noexcept
{
    if (text[0] != 'c' || text[1] != 'p' || text[2] != 'u' || text[3] != ' ')
        return false;
    const char* p = text + 3;
    // user nice system idle iowait irq softirq steal. guest / guest_nice are already counted in user / nice.
    std::uint64_t field[8] {};
    int fields = 0;
    while (fields < 8)
    {
        while (*p == ' ')
            ++p;
        if (*p < '0' || *p > '9')
            break;
        std::uint64_t v = 0;
        while (*p >= '0' && *p <= '9')
            v = v * 10 + static_cast<std::uint64_t>(*p++ - '0');
        field[fields++] = v;
    }
    if (fields < 4) // kernels before 2.6 have no iowait / irq / softirq; the rest are zero then
        return false;
    const std::uint64_t idle = field[3] + field[4];
    total = 0;
    for (const auto v : field)
        total += v;
    busy = total - idle;
    return true;
}

float CpuUsage::share(std::uint64_t busy0, std::uint64_t total0, std::uint64_t busy1, std::uint64_t total1) noexcept
{
    if (total1 <= total0)
        return -1;
    const std::uint64_t dt = total1 - total0;
    // The per-core iowait counter can step back (a task migrates between cores): clamp to [0, dt].
    const std::uint64_t db = busy1 <= busy0 ? 0 : (busy1 - busy0 > dt ? dt : busy1 - busy0);
    return static_cast<float>(100.0 * static_cast<double>(db) / static_cast<double>(dt));
}

bool CpuUsage::sample() noexcept
{
    std::uint64_t busy = 0, total = 0;
    if (! readCounters(busy, total))
        return false;
    if (total_ != 0)
    {
        const float p = share(busy_, total_, busy, total);
        if (p < 0) // no tick elapsed: keep the baseline, so the next sample spans the longer interval
            return true;
        percent_ = p;
    }
    busy_ = busy;
    total_ = total;
    return true;
}
} // namespace core
