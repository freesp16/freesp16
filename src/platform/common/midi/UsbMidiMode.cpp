#include "platform/common/midi/UsbMidiMode.h"

#include "ProjectIdentity.h"

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>

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

int runCommand(const std::vector<std::string>& argv, const char* cwd, int timeoutMs)
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
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
#ifdef POSIX_SPAWN_USEVFORK
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_USEVFORK); // no copy of this (large) process's page tables
#endif
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
        if (waited >= timeoutMs)
        {
            ::kill(pid, SIGKILL);
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
    e.run = [](const std::vector<std::string>& argv) { return runCommand(argv, "/root/pdj", 30000); };
    e.log = [](const std::string& text) {
        std::printf("[sp16-mod] %s MIDI USB %s\n", identity::envPrefix, text.c_str());
        std::fflush(stdout);
    };
    return e;
}

UsbMidiMode::~UsbMidiMode() { wait(); }

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
    worker_ = std::thread([this, argv, line] {
        const int status = env_.run(argv);
        const UsbGadget after = env_.probe();
        env_.log("switch status=" + std::to_string(status) + " gadget=" + toString(after) +
                 (after == UsbGadget::Midi ? " ok" : " FAILED"));
        settled_ = true;
    });
}
} // namespace platform::unitmidi
