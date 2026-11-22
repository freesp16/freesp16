// Worker threads of the library (two, for decoding, listings and file operations) and the
// message-thread completion queue their results come back through.
//
//   message thread ── run(job, done) ──▶ worker: job() ──▶ completion queue ──▶ message thread: pump() → done()
//
// There is no shared services::WorkerPool yet, so the library has its own; a shared pool could back Workers later
// without changing a caller. Jobs never run on the audio thread; pump() is called by the owner's frame loop (or
// a juce::Timer), tests call drain(). A Cancel token (shared flag) lets a superseded job stop early (preview focus
// changes, LIB-011).
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace services::library
{
using CancelToken = std::shared_ptr<std::atomic<bool>>;
inline CancelToken makeCancelToken()
{
    return std::make_shared<std::atomic<bool>>(false);
}

class Workers
{
public:
    explicit Workers(int threads = 2);
    ~Workers(); // finishes running jobs, drops queued ones and pending completions
    Workers(const Workers&) = delete;
    Workers& operator=(const Workers&) = delete;

    // Message thread: runs `job` on a worker; `done` (optional) is queued for the message thread when it finishes.
    void run(std::function<void()> job, std::function<void()> done = {});
    // Any thread: queue a callback for the message thread.
    void post(std::function<void()> onMessageThread);

    // Message thread: runs every queued completion; returns how many ran.
    std::size_t pump();
    // Message thread (tests, shutdown): pumps until no job is queued or running and no completion is pending.
    void drain();
    bool idle() const;

    int threads() const noexcept { return static_cast<int>(threads_.size()); }

private:
    void loop();

    mutable std::mutex mutex_;
    std::condition_variable wake_, quiet_;
    std::deque<std::function<void()>> jobs_;
    std::deque<std::function<void()>> completions_;
    int running_ = 0;
    bool stopping_ = false;
    std::vector<std::thread> threads_;
};
} // namespace services::library
