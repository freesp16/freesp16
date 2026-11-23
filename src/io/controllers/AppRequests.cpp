#include "io/controllers/AppRequests.h"

#include <atomic>
#include <deque>
#include <mutex>

namespace io
{
namespace
{
std::atomic<bool> quit {false}, standby {false};

struct Shots
{
    std::mutex mutex;
    std::deque<std::string> pending;
};
Shots& shots()
{
    static Shots s;
    return s;
}
Shots& commands()
{
    static Shots s;
    return s;
}
} // namespace

void requestQuit() noexcept { quit.store(true, std::memory_order_relaxed); }
bool quitRequested() noexcept { return quit.load(std::memory_order_relaxed); }
void clearQuitRequest() noexcept { quit.store(false, std::memory_order_relaxed); }

void requestStandby() noexcept { standby.store(true, std::memory_order_relaxed); }
bool standbyRequested() noexcept { return standby.load(std::memory_order_relaxed); }
void clearStandbyRequest() noexcept { standby.store(false, std::memory_order_relaxed); }

void requestScreenshot(const std::string& path)
{
    auto& s = shots();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.pending.push_back(path);
}

bool takeScreenshotRequest(std::string& path)
{
    auto& s = shots();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.pending.empty())
        return false;
    path = std::move(s.pending.front());
    s.pending.pop_front();
    return true;
}
void requestAppCommand(const std::string& text)
{
    auto& s = commands();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.pending.push_back(text);
}

bool takeAppCommand(std::string& text)
{
    auto& s = commands();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.pending.empty())
        return false;
    text = std::move(s.pending.front());
    s.pending.pop_front();
    return true;
}
} // namespace io
