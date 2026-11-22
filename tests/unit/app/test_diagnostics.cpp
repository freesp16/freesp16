// The start-up DIAG helpers (app/Diagnostics.h) and the fatal-signal report: a forked child crashes
// (SIGSEGV, abort) with the handler installed; its stdout must carry the CRASH header, a backtrace and the end line,
// and the child must still die of that signal (the exit status the device's launcher logs).
#include "app/Diagnostics.cpp"

#include "tests/fakes/FakeAudioDevice.h"

#include <catch2/catch_test_macros.hpp>

#include <sys/wait.h>

#include <csignal>
#include <string>
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
#if defined(__linux__)
    CHECK(app::diag::residentKb() > 0);
    CHECK(app::diag::peakResidentKb() >= app::diag::residentKb());
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
