// The start-up DIAG helpers (app/Diagnostics.h) and the fatal-signal report: a forked child crashes
// (SIGSEGV, abort) with the handler installed; its stdout must carry the CRASH header, a backtrace and the end line,
// and the child must still die of that signal (the exit status the device's launcher logs).
#include "app/Diagnostics.cpp"

#include "core/util/ThreadRole.h"
#include "tests/fakes/FakeAudioDevice.h"

#include <catch2/catch_test_macros.hpp>

#include <sys/wait.h>

#include <atomic>
#include <csignal>
#include <string>
#include <thread>
#include <vector>

namespace
{
// Runs `crash` in a child with the handler installed; returns its stdout and its wait status.
std::string crashChild(void (*crash)(), int& status)
{
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0)
    {
        ::dup2(fds[1], STDOUT_FILENO);
        ::close(fds[0]);
        app::diag::installCrashHandler("[tag] TEST");
        crash();
        ::_exit(0); // not reached
    }
    ::close(fds[1]);
    std::string out;
    char buf[4096];
    for (ssize_t n; (n = ::read(fds[0], buf, sizeof buf)) > 0;)
        out.append(buf, static_cast<std::size_t>(n));
    ::close(fds[0]);
    ::waitpid(pid, &status, 0);
    return out;
}

[[gnu::noinline]] void segv()
{
    ::raise(SIGSEGV);
}
[[gnu::noinline]] void abortNow()
{
    std::abort();
}
} // namespace

TEST_CASE("app diag: the audio port map line", "[app][diag]")
{
    fakes::FakeAudioDevice device; // MAIN 0/1, PHONES 2/3, the rest absent
    CHECK(app::diag::describePorts(device)
          == "MAIN=0/1 PHONES=2/3 OUT3-4=absent OUT5-6=absent OUT7-8=absent FILTERSEND=absent");
}

TEST_CASE("app diag: system lines, mounts, memory", "[app][diag]")
{
    std::vector<std::string> lines;
    platform::Paths paths;
    paths.dataDir = "/tmp";
    paths.cacheDir = "/nonexistent-cache-dir";
    app::diag::systemLines(paths, [&](const std::string& l) { lines.push_back(l); });
    auto has = [&](const char* prefix) {
        for (const auto& l : lines)
            if (l.rfind(prefix, 0) == 0)
                return true;
        return false;
    };
    CHECK(has("kernel "));
    CHECK(has("cpu online="));
    CHECK(has("libc "));
    CHECK(has("memory "));
    CHECK(has("data dir=/tmp writable"));
    CHECK(has("cache dir=/nonexistent-cache-dir missing"));
    CHECK(has("storage fat_mounts="));
    CHECK(has("midi rawmidi="));
    CHECK(has("midi unit din_midi="));
#if defined(__linux__)
    const auto resident = app::diag::residentKb();
    CHECK(resident > 0);
    CHECK(app::diag::peakResidentKb() >= resident);
    CHECK(app::diag::freeBytes("/tmp") > 0);
    CHECK(app::diag::mountOf("/tmp/x/y").size() > 2);
#endif
    CHECK(app::diag::freeBytes("/nonexistent/path") == -1);
}

TEST_CASE("app diag: a fatal signal writes the CRASH report and still kills the process", "[app][diag]")
{
    int status = 0;
    const std::string out = crashChild(&segv, status);
    INFO(out);
    CHECK(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGSEGV);
    CHECK(out.find("[tag] TEST CRASH SIGSEGV (11)") != std::string::npos);
    CHECK(out.find("[tag] TEST CRASH end") != std::string::npos);
#if defined(__GLIBC__)
    CHECK(out.find("[tag] TEST CRASH backtrace frames=") != std::string::npos);
#endif
#if defined(__linux__)
    CHECK(out.find("[tag] TEST CRASH map ") != std::string::npos);
    CHECK(out.find(" tid=") != std::string::npos);
#endif

    const std::string aborted = crashChild(&abortNow, status);
    INFO(aborted);
    CHECK(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGABRT);
    CHECK(aborted.find("[tag] TEST CRASH SIGABRT (6)") != std::string::npos);
    CHECK(aborted.find("[tag] TEST CRASH end") != std::string::npos);
}

#if defined(__linux__)
TEST_CASE("app diag: one thread line per thread, with its policy, nice value and cores", "[app][diag]")
{
    std::atomic<bool> ready {false}, done {false};
    std::thread worker(
        [&]
        {
            core::threads::apply(core::threads::Role::Worker, "diag-worker"); // nice 10, inherited cores
            ready = true;
            while (! done)
                std::this_thread::yield();
        });
    while (! ready)
        std::this_thread::yield();
    std::vector<std::string> lines;
    app::diag::threadLines([&](const std::string& l) { lines.push_back(l); });
    done = true;
    worker.join();
    REQUIRE(lines.size() >= 2);
    std::string mine;
    for (const auto& l : lines)
    {
        CHECK(l.rfind("thread ", 0) == 0);
        CHECK(l.find(" cpus=") != std::string::npos);
        if (l.rfind("thread diag-worker ", 0) == 0)
            mine = l;
    }
    INFO(mine);
    CHECK(mine.find(" policy=OTHER prio=0 nice=10 cpus=") != std::string::npos);
}

TEST_CASE("app diag: the interrupt lines group interrupts by their cores", "[app][diag]")
{
    std::vector<std::string> lines;
    app::diag::irqLines([&](const std::string& l) { lines.push_back(l); });
    // A container may hide /proc/irq: no line then; otherwise the summary comes first.
    if (! lines.empty())
        CHECK(lines.front().rfind("irq cpus ", 0) == 0);
}
#endif
