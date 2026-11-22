// Whole-machine CPU usage, from the kernel's own time accounting: the busy share of every core's time between two
// samples (Linux: the aggregate "cpu" line of /proc/stat, busy = user + nice + system + irq + softirq + steal, idle =
// idle + iowait; macOS: HOST_CPU_LOAD_INFO). 100 % = every core busy for the whole interval. The accuracy is the
// kernel tick (10 ms per core at HZ=100), so over a 1 s interval on two cores one tick is 0.5 %.
//
//   sample()    reads the counters (one open/read/close of /proc/stat, no allocation) and updates percent() from the
//               previous sample; the first call after construction or reset() only sets the baseline
//   percent()   the busy share 0–100 between the last two samples, -1 until there is one
//
// Message thread (or any one thread). Never on the audio thread: sample() is a system call.
#pragma once

#include <cstdint>

namespace core
{
class CpuUsage
{
public:
    bool sample() noexcept; // false = the counters cannot be read (no /proc, another OS): percent() stays -1
    float percent() const noexcept { return percent_; }
    void reset() noexcept
    {
        busy_ = total_ = 0;
        percent_ = -1;
    }

    // The busy share between two counter pairs, 0–100 (a counter that went backwards clamps; no elapsed time = -1).
    static float share(std::uint64_t busy0, std::uint64_t total0, std::uint64_t busy1, std::uint64_t total1) noexcept;
    // Parses the aggregate "cpu" line of /proc/stat into busy and total ticks; false if `text` does not start with it.
    static bool parseProcStat(const char* text, std::uint64_t& busy, std::uint64_t& total) noexcept;

private:
    std::uint64_t busy_ = 0, total_ = 0; // total_ == 0: no baseline yet
    float percent_ = -1;
};
} // namespace core
