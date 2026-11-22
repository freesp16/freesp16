// Device side of the bench harness (BENCH_STANDALONE, built by tests/bench/harness/device/ for
// scripts/emulator-bench.sh): runs the registered benches and prints each result as an app log line,
// "[sp16-mod] <PREFIX> BENCH ...", which the emulator copies into the session's console.txt; then exits.
//   <binary> [filter]     filter: a bench name, or a prefix ending in '*'; empty = every bench compiled in
// Without an argument the filter comes from <PREFIX>_BENCH or the line `bench=<filter>` of the app's settings file
// (/opt/sp16/dev-share/<name>.conf in the emulator's developer overlay, written by scripts/install.sh).
#if defined(BENCH_STANDALONE)
#include "tests/bench/harness/Bench.h"

#include "ProjectIdentity.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{
void logLine(const char* text)
{
    std::printf("[sp16-mod] %s %s\n", identity::envPrefix, text);
    std::fflush(stdout);
}

std::string settingsFilter()
{
    if (const char* v = std::getenv((std::string(identity::envPrefix) + "_BENCH").c_str()))
        return v;
    const std::string path = std::string("/opt/sp16/dev-share/") + identity::name + ".conf";
    std::string filter;
    if (std::FILE* f = std::fopen(path.c_str(), "r"))
    {
        char line[512];
        while (std::fgets(line, sizeof line, f) != nullptr)
            if (std::strncmp(line, "bench=", 6) == 0)
            {
                filter = line + 6;
                while (! filter.empty() && (filter.back() == '\n' || filter.back() == '\r'))
                    filter.pop_back();
            }
        std::fclose(f);
    }
    return filter;
}

bool matches(const std::string& filter, const char* name)
{
    if (filter.empty())
        return true;
    if (filter.back() == '*')
        return std::strncmp(name, filter.c_str(), filter.size() - 1) == 0;
    return filter == name;
}
} // namespace

int main(int argc, char** argv)
{
    const std::string filter = argc > 1 ? argv[1] : settingsFilter();
    char line[600];
    std::snprintf(line, sizeof line, "BENCH START benches=%d filter=%s", bench::count(),
                  filter.empty() ? "(all)" : filter.c_str());
    logLine(line);
    int ran = 0, over = 0, failed = 0;
    for (int i = 0; i < bench::count(); ++i)
    {
        const auto& e = bench::entry(i);
        if (! matches(filter, e.name))
            continue;
        const auto r = bench::run(e);
        bench::format(r, line, sizeof line);
        logLine(line);
        ++ran;
        over += r.ran && r.over();
        failed += ! r.ran;
    }
    std::snprintf(line, sizeof line, "BENCH DONE ran=%d over=%d failed=%d (emulator timings are relative only)", ran,
                  over, failed);
    logLine(line);
    return ran > 0 && failed == 0 ? 0 : 1;
}
#endif
