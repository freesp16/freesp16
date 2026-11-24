#include "platform/common/midi/UsbMidiMode.h"

#include "ProjectIdentity.h"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>

extern char** environ;

namespace platform::unitmidi
{
UsbGadget usbGadget(const std::string& sysModules)
{
    if (::access((sysModules + "/g_midi2").c_str(), F_OK) == 0)
        return UsbGadget::Midi;
    if (::access((sysModules + "/g_mass_storage2").c_str(), F_OK) == 0)
        return UsbGadget::Storage;
    return UsbGadget::None;
}

const char* toString(UsbGadget g) noexcept
{
    return g == UsbGadget::Midi ? "midi" : g == UsbGadget::Storage ? "storage" : "none";
}

bool hostConfigured(const std::string& udcClass)
{
    DIR* d = ::opendir(udcClass.c_str());
    if (d == nullptr)
        return false;
    bool configured = false;
    while (const dirent* e = ::readdir(d))
    {
        if (e->d_name[0] == '.')
            continue;
        std::ifstream f(udcClass + "/" + e->d_name + "/state");
        std::string state;
        if (std::getline(f, state) && state.rfind("configured", 0) == 0)
            configured = true;
    }
    ::closedir(d);
    return configured;
}

std::vector<std::string> midiModeCommand(UsbGadget now, bool scriptPresent)
{
    switch (now)
    {
        case UsbGadget::Midi: return {};
        case UsbGadget::Storage: // the disk goes back to /mnt/emmc: the unit's own script knows how
            return scriptPresent ? std::vector<std::string> {kUsbGadgetScript, "midi", "probe"}
                                 : std::vector<std::string> {};
        case UsbGadget::None: return {"modprobe", "g_midi2"};
    }
    return {};
}

void closeOnExecAll() noexcept
{
    DIR* d = ::opendir("/proc/self/fd");
    if (d == nullptr)
        return;
    const int own = ::dirfd(d);
    while (const dirent* e = ::readdir(d))
    {
        char* end = nullptr;
        const long fd = std::strtol(e->d_name, &end, 10);
        if (end == e->d_name || *end != '\0' || fd <= 2 || fd == own)
            continue;
        const int flags = ::fcntl(static_cast<int>(fd), F_GETFD);
        if (flags >= 0 && (flags & FD_CLOEXEC) == 0)
            ::fcntl(static_cast<int>(fd), F_SETFD, flags | FD_CLOEXEC);
    }
    ::closedir(d);
}

int runCommand(const std::vector<std::string>& argv, const char* cwd, int timeoutMs, const std::atomic<bool>* abandon)
{
    if (argv.empty())
        return -1;
    // posix_spawn has no working-directory attribute on glibc 2.13: through sh, which also does the PATH lookup.
    std::vector<std::string> shArgv {"/bin/sh", "-c", "cd \"$0\" && exec \"$@\"", cwd != nullptr ? cwd : "/"};
    shArgv.insert(shArgv.end(), argv.begin(), argv.end());
    std::vector<char*> sh;
    for (auto& a : shArgv)
        sh.push_back(a.data());
    sh.push_back(nullptr);
    closeOnExecAll(); // ALSA, the display, the library's files…: none of them is the script's
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    short flags = POSIX_SPAWN_SETPGROUP; // its own group (pgid = its pid): a timeout kills the script's children too
#ifdef POSIX_SPAWN_USEVFORK
    flags |= POSIX_SPAWN_USEVFORK; // no copy of this (large) process's page tables
#endif
    posix_spawnattr_setflags(&attr, flags);
    posix_spawnattr_setpgroup(&attr, 0);
    pid_t pid = -1;
    const int r = posix_spawn(&pid, "/bin/sh", nullptr, &attr, sh.data(), environ);
    posix_spawnattr_destroy(&attr);
    if (r != 0)
        return -1;
    for (int waited = 0;; waited += 20)
    {
        int status = 0;
        const pid_t w = ::waitpid(pid, &status, WNOHANG);
        if (w == pid)
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (w < 0)
            return -1;
        if (abandon != nullptr && abandon->load())
            return kAbandoned; // left to finish on its own (init reaps it once this process is gone)
        if (waited >= timeoutMs)
        {
            ::kill(-pid, SIGKILL); // the group
            ::waitpid(pid, &status, 0);
            return -1;
        }
        const timespec ts {0, 20 * 1000000L};
        ::nanosleep(&ts, nullptr);
    }
}

UsbMidiMode::Env UsbMidiMode::unitEnv()
{
    Env e;
    e.probe = [] { return usbGadget(); };
    e.exists = [](const char* path) { return ::access(path, X_OK) == 0; };
    e.hostAttached = [] { return hostConfigured(); };
    e.run = [](const std::vector<std::string>& argv, const std::atomic<bool>& abandon)
    { return runCommand(argv, "/root/pdj", 30000, &abandon); };
    e.log = [](const std::string& text) {
        std::printf("[sp16-mod] %s MIDI USB %s\n", identity::envPrefix, text.c_str());
        std::fflush(stdout);
    };
    return e;
}

UsbMidiMode::~UsbMidiMode()
{
    abandon_ = true; // the runner returns within one poll (20 ms)
    wait();
}

void UsbMidiMode::wait()
{
    if (worker_.joinable())
        worker_.join();
}

void UsbMidiMode::tick()
{
    if (decided_)
        return;
    const UsbGadget now = env_.probe();
    if (now == UsbGadget::Midi)
    {
        decided_ = true;
        settled_ = true;
        env_.log("gadget=midi");
        return;
    }
    if (++ticks_ < grace_) // the unit's start-up script may still be loading the gadget
        return;
    decided_ = true;
    if (now == UsbGadget::Storage && env_.hostAttached && env_.hostAttached())
    {
        settled_ = true; // never pull the disk from under a computer that may have it mounted
        env_.log("gadget=storage host=configured: left in storage mode (a computer has the disk; switch by hand)");
        return;
    }
    const auto argv = midiModeCommand(now, env_.exists(kUsbGadgetScript));
    if (argv.empty())
    {
        settled_ = true;
        env_.log(std::string("gadget=") + toString(now) + " switch=impossible (" + kUsbGadgetScript + " missing)");
        return;
    }
    std::string line;
    for (const auto& a : argv)
        line += (line.empty() ? "" : " ") + a;
    env_.log(std::string("gadget=") + toString(now) + " switching to midi: " + line);
    worker_ = std::thread(
        [this, argv, line]
        {
            const int status = env_.run(argv, abandon_);
            if (status == kAbandoned)
            {
                env_.log("switch abandoned (quitting): " + line + " left to finish");
                settled_ = true;
                return;
            }
            const UsbGadget after = env_.probe();
            env_.log("switch status=" + std::to_string(status) + " gadget=" + toString(after) +
                     (after == UsbGadget::Midi ? " ok" : " FAILED"));
            settled_ = true;
        });
}
} // namespace platform::unitmidi
