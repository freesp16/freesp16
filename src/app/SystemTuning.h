// System tuning for the audio path (platform::Tuning): code locked in RAM, the CPU frequency governor. Message thread,
// start-up and exit only; every step is best effort and reports what it did as one log line.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace app::tuning
{
// mlock() every file-backed mapping that holds code (an executable mapping) and every mapping of the executable
// itself (its read-only data and tables too), so none of it is dropped and read back from the medium — a USB stick —
// on the audio thread. Anonymous memory is not touched: without swap it is never paged out (the line reports the
// swap size), and malloc_trim keeps returning freed heap pages. Needs CAP_IPC_LOCK or a large RLIMIT_MEMLOCK.
struct CodeLock
{
    long lockedKb = 0, failedKb = 0;
    int mappings = 0, failed = 0;
    int firstError = 0; // errno of the first failure
    long swapKb = -1;   // SwapTotal (−1 = unknown)
};
CodeLock lockCode();
std::string describe(const CodeLock&);

// Sets `name` on every CPU's cpufreq policy that offers it; restore() puts the previous governors back (exit).
struct Governor
{
    std::string before; // cpu0's, "" = no cpufreq
    bool changed = false;
    int written = 0, failed = 0;
    std::string note;
    std::vector<std::pair<std::string, std::string>> previous; // {scaling_governor file, its value before}
};
Governor setGovernor(const std::string& name);
void restore(const Governor&);
std::string describe(const Governor&, const std::string& wanted);

// kernel.sched_rt_runtime_us (/proc/sys/kernel/sched_rt_runtime_us): the real-time threads' share of every
// sched_rt_period_us on each core, −1 = no limit. Set while the app runs; restore() puts the previous value back.
struct RtRuntime
{
    std::string before; // "" = not readable
    bool changed = false;
    std::string note;
};
RtRuntime setRtRuntime(const std::string& value); // "" = leave it
void restore(const RtRuntime&);
std::string describe(const RtRuntime&, const std::string& wanted);
} // namespace app::tuning
