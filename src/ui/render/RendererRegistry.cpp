#include "ui/render/RendererRegistry.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace ui::render
{
namespace
{
std::vector<RendererEntry>& entries()
{
    static std::vector<RendererEntry> list;
    return list;
}

double nowMs() noexcept
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}
} // namespace

void registerRenderer(std::string_view name, RendererFactory factory, bool childProbe)
{
    auto& list = entries();
    for (auto& e : list)
        if (e.name == name)
        {
            e = {name, factory, childProbe};
            return;
        }
    list.push_back({name, factory, childProbe});
}

std::vector<RendererEntry> registeredRenderers()
{
    auto list = entries();
    std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return list;
}

const RendererEntry* findRenderer(std::string_view name) noexcept
{
    for (const auto& e : entries())
        if (e.name == name)
            return &e;
    return nullptr;
}

bool needsChildProbe(std::string_view name) noexcept
{
    const auto* e = findRenderer(name);
    return e != nullptr && e->needsChildProbe;
}

std::unique_ptr<Renderer> create(std::string_view name, const RendererEnv& env, std::string& error)
{
    const auto* e = findRenderer(name);
    if (e == nullptr || e->create == nullptr)
    {
        error = "unknown backend " + std::string(name);
        return nullptr;
    }
    return e->create(env, error);
}

bool probeInChild(std::string_view name, const RendererEnv& env, const std::function<bool(Renderer&)>& work,
                  double timeoutMs, std::string& why, const std::function<void(const std::string&)>& onCreateError)
{
    std::fflush(stdout);
    std::fflush(stderr);
    const pid_t pid = fork();
    if (pid == 0)
    {
        std::string error;
        auto r = create(name, env, error);
        if (! r)
        {
            if (onCreateError)
                onCreateError(error);
            std::fflush(stdout);
            _exit(2);
        }
        const bool ok = ! work || work(*r);
        std::fflush(stdout);
        _exit(ok ? 0 : 3);
    }
    if (pid < 0)
        return why = "fork failed", false;
    const double deadline = nowMs() + timeoutMs;
    while (nowMs() < deadline)
    {
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid)
        {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
                return true;
            why = WIFSIGNALED(status) ? "child killed by signal " + std::to_string(WTERMSIG(status))
                                      : "child exit " + std::to_string(WEXITSTATUS(status));
            return false;
        }
        usleep(50000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    return why = "child hung (killed)", false;
}

std::unique_ptr<Renderer> createFirstWorking(std::span<const std::string> order, const RendererEnv& env,
                                             std::string& log)
{
    for (const auto& name : order)
    {
        std::string why;
        if (needsChildProbe(name) && ! probeInChild(name, env, {}, 10000.0, why))
        {
            log += name + " unavailable: " + why + "\n";
            continue;
        }
        auto r = create(name, env, why);
        if (r)
        {
            log += name + " selected\n";
            return r;
        }
        log += name + " unavailable: " + why + "\n";
    }
    return nullptr;
}
} // namespace ui::render
