#include "services/library/Workers.h"

#include <utility>

namespace services::library
{
Workers::Workers(int threads)
{
    for (int i = 0; i < (threads < 1 ? 1 : threads); ++i)
        threads_.emplace_back([this] { loop(); });
}

Workers::~Workers()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        jobs_.clear();
    }
    wake_.notify_all();
    for (auto& t : threads_)
        t.join();
}

void Workers::run(std::function<void()> job, std::function<void()> done)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_)
            return;
        jobs_.push_back(
            [this, job = std::move(job), done = std::move(done)]() mutable
            {
                job();
                if (done)
                    post(std::move(done));
            });
    }
    wake_.notify_one();
}

void Workers::post(std::function<void()> onMessageThread)
{
    std::lock_guard<std::mutex> lock(mutex_);
    completions_.push_back(std::move(onMessageThread));
    quiet_.notify_all();
}

std::size_t Workers::pump()
{
    std::deque<std::function<void()>> ready;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ready.swap(completions_);
    }
    for (auto& fn : ready)
        fn();
    return ready.size();
}

bool Workers::idle() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return jobs_.empty() && running_ == 0 && completions_.empty();
}

void Workers::drain()
{
    for (;;)
    {
        pump();
        std::unique_lock<std::mutex> lock(mutex_);
        if (jobs_.empty() && running_ == 0 && completions_.empty())
            return;
        quiet_.wait(lock, [this] { return ! completions_.empty() || (jobs_.empty() && running_ == 0); });
    }
}

void Workers::loop()
{
    for (;;)
    {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || ! jobs_.empty(); });
            if (stopping_)
                return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
            ++running_;
        }
        job();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            --running_;
        }
        quiet_.notify_all();
    }
}
} // namespace services::library
