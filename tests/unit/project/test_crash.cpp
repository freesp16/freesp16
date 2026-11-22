// NFR-010 / PRJ-010: 1,000 fork-and-kill iterations. A child process saves the other of two project versions
// (with the backup and rotation steps) and is SIGKILLed at a random point of the save; afterwards the bundle consists
// (after the recovery every open runs) of exactly the old or exactly the new version's files — never torn — and
// nothing is left beside it. The child is this test executable re-run (fork + exec, posix_spawn) in the hidden test
// "project.crash-child": macOS aborts a forked child that touches CoreFoundation (JUCE's file layer) without exec.
// It signals on a pipe right before it starts the save, so the kill delay is measured from the save's start.
#include "ProjectTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

using namespace projtest;

namespace
{
using Files = std::map<std::string, std::string>;

// The files a version must consist of, exactly (deterministic: fixed timestamps).
Files expectedFiles(const m::Project& p, const std::string& timestamp)
{
    st::SaveOptions o;
    o.timestamp = timestamp;
    Files out;
    for (auto& f : st::NativeCodec::encode(p, o))
        out[f.name] = std::move(f.text);
    return out;
}
constexpr std::uint32_t kSeedA = 101, kSeedB = 202;
const std::string kStamp = "2026-11-21T12:00:00.000Z";

std::unique_ptr<m::Project> version(std::uint32_t seed) { return storetest::randomProject(seed, 0.05); }

st::WriteOptions writeOptions(const juce::File& backups)
{
    st::WriteOptions options;
    options.backupFolder = backups;
    options.backupsKept = 10;
    options.timestamp = kStamp;
    return options;
}

// Starts the child saving version `seed`; returns its pid, the read end of its "save starts" pipe in `startFd`.
pid_t spawnSave(const juce::File& bundle, const juce::File& backups, std::uint32_t seed, int& startFd)
{
    int fds[2];
    if (::pipe(fds) != 0)
        return -1;
    const auto exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName().toStdString();
    const std::string env[] = {"CRASH_BUNDLE=" + bundle.getFullPathName().toStdString(),
                               "CRASH_BACKUPS=" + backups.getFullPathName().toStdString(),
                               "CRASH_SEED=" + std::to_string(seed), "CRASH_FD=3"};
    std::vector<char*> envp;
    for (const auto& e : env)
        envp.push_back(const_cast<char*>(e.c_str()));
    for (char** e = environ; *e != nullptr; ++e)
        if (std::strncmp(*e, "CRASH_", 6) != 0)
            envp.push_back(*e);
    envp.push_back(nullptr);
    std::string name = "project.crash-child";
    char* argv[] = {const_cast<char*>(exe.c_str()), name.data(), nullptr};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 3);
    posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    pid_t pid = -1;
    const int r = ::posix_spawn(&pid, exe.c_str(), &actions, nullptr, argv, envp.data());
    posix_spawn_file_actions_destroy(&actions);
    ::close(fds[1]);
    if (r != 0)
    {
        ::close(fds[0]);
        return -1;
    }
    startFd = fds[0];
    return pid;
}
} // namespace

// The child side: saves one version and exits; never listed (hidden), run only by spawnSave.
TEST_CASE("project.crash-child", "[.crashchild]")
{
    const char* bundle = std::getenv("CRASH_BUNDLE");
    const char* backups = std::getenv("CRASH_BACKUPS");
    const char* seed = std::getenv("CRASH_SEED");
    if (bundle == nullptr || backups == nullptr || seed == nullptr)
        SKIP("run by project.crash only");
    const auto p = version(static_cast<std::uint32_t>(std::atoi(seed)));
    const auto options = writeOptions(juce::File(backups));
    if (::write(3, "S", 1) != 1) // the "started" byte for the parent; a lost byte only makes it wait longer
    {
    }
    const auto r = st::writeProjectVersion(*p, juce::File(bundle), options);
    ::_exit(r ? 0 : 3);
}

TEST_CASE("project.crash: 1,000 fork-and-kill saves leave the old or the new version, never a torn one",
          "[project][long]")
{
    libtest::TempDir tmp("forkkill");
    const auto bundle = tmp.dir().getChildFile("Kill.sixteen");
    const auto backups = tmp.dir().getChildFile("Backups/Kill");
    const auto options = writeOptions(backups);
    const auto a = version(kSeedA);
    const auto b = version(kSeedB);
    const Files filesA = expectedFiles(*a, kStamp), filesB = expectedFiles(*b, kStamp);
    REQUIRE(filesA != filesB);
    st::SaveOptions first;
    first.timestamp = kStamp;
    REQUIRE(st::saveProject(*a, bundle, first).ok());
    REQUIRE(tree(bundle) == filesA);

    // The duration of one save in a child, from its start signal to its exit (cold process, real fsyncs, the load
    // of the machine): the kill delays are spread over it (and a bit beyond). Measured on children rather than in this
    // process, whose warm saves can be several times faster (Linux ext4 vs APFS) and would kill nearly every child
    // before it commits.
    std::vector<long long> childUs;
    for (int i = 0; i < 5; ++i)
    {
        int startFd = -1;
        const pid_t pid = spawnSave(bundle, backups, i % 2 ? kSeedA : kSeedB, startFd);
        REQUIRE(pid > 0);
        char c = 0;
        REQUIRE(::read(startFd, &c, 1) == 1);
        const auto t0 = std::chrono::steady_clock::now();
        ::close(startFd);
        int status = 0;
        REQUIRE(::waitpid(pid, &status, 0) == pid);
        REQUIRE((WIFEXITED(status) && WEXITSTATUS(status) == 0));
        childUs.push_back(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
    }
    std::sort(childUs.begin(), childUs.end());
    const auto saveUs = childUs[childUs.size() / 2];
    std::printf("project.crash: one save takes about %lld us\n", static_cast<long long>(saveUs));
    bool currentIsA = false; // the fifth save wrote b

    std::uint32_t x = 0x9E3779B9u;
    const auto random = [&x]
    {
        x ^= x << 13, x ^= x >> 17, x ^= x << 5;
        return x;
    };
    int keptOld = 0, gotNew = 0, finished = 0;
    auto loaded = std::make_unique<m::Project>();
    for (int i = 0; i < 1000; ++i)
    {
        const bool nextIsA = ! currentIsA;
        int startFd = -1;
        const pid_t pid = spawnSave(bundle, backups, nextIsA ? kSeedA : kSeedB, startFd);
        REQUIRE(pid > 0);
        char c = 0;
        const bool started = ::read(startFd, &c, 1) == 1; // the child is about to save
        ::close(startFd);
        if (started)
            ::usleep(static_cast<useconds_t>(random() % static_cast<std::uint32_t>(saveUs * 12 / 10 + 1)));
        ::kill(pid, SIGKILL);
        int status = 0;
        REQUIRE(::waitpid(pid, &status, 0) == pid);
        INFO("iteration " << i << ", child status " << status);
        REQUIRE(started);
        if (WIFEXITED(status))
        {
            REQUIRE(WEXITSTATUS(status) == 0); // a save that finished must have succeeded
            ++finished;
        }
        else
            REQUIRE((WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL));

        // What the next open does first (openProjectInto runs the same recovery).
        (void) st::AtomicWriter::recover(bundle);
        const auto files = tree(bundle);
        const bool isNew = files == (nextIsA ? filesA : filesB);
        const bool isOld = files == (currentIsA ? filesA : filesB);
        REQUIRE((isNew || isOld)); // every file of one version, nothing of the other, nothing else
        if (isNew)
        {
            currentIsA = nextIsA;
            ++gotNew;
        }
        else
            ++keptOld;
        if (i % 100 == 0) // and it opens as that model
        {
            st::ImportReport report;
            REQUIRE(st::openProjectInto(bundle, *loaded, report).ok());
            REQUIRE(*loaded == (currentIsA ? *a : *b));
        }
        // Nothing beside the bundle but the backups folder (staging, sealed and old copies are recovered).
        for (const auto& e : juce::RangedDirectoryIterator(tmp.dir(), false, "*", juce::File::findFilesAndDirectories))
        {
            const auto name = e.getFile().getFileName();
            INFO("leftover " << name);
            REQUIRE((name == "Kill.sixteen" || name == "Backups"));
        }
        if (i % 50 == 0) // backups stay complete bundles and bounded (+1 when a kill hit before the rotation)
            REQUIRE(st::listBackups(backups).size() <= 11);
    }
    std::printf("project.crash: kept the old version %d times, got the new one %d times, %d saves finished\n",
                keptOld, gotNew, finished);
    CHECK(keptOld > 50);
    CHECK(gotNew > 50);
    for (const auto& backup : st::listBackups(backups))
    {
        const auto files = tree(backup);
        CHECK((files == filesA || files == filesB));
    }
}
