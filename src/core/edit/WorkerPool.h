// Where the SceneEditor prepares large copies (EDT-004). core has no threads of
// its own (src/core/CMakeLists.txt) and may not include services/, so the editor sees this interface; the composition
// root adapts its worker threads to it (services::library::Workers has exactly this run(job, done) shape):
//
//   core::edit::WorkerPoolAdapter<services::library::Workers> pool {workers};
//   core::edit::setSharedWorkers(&pool);   // core/edit/SharedEditor.h
//
// Contract: run() is called on the message thread; `job` runs on some other thread (or inline) and touches only what
// it captured; `done` runs later on the message thread (the owner's completion pump). InlineWorkerPool runs both at
// once on the caller's thread (tests, tools, a build without workers): edits are then synchronous.
#pragma once

#include <functional>
#include <utility>

namespace core::edit
{
class WorkerPool
{
public:
    virtual ~WorkerPool() = default;
    virtual void run(std::function<void()> job, std::function<void()> done) = 0;
};

class InlineWorkerPool final : public WorkerPool
{
public:
    void run(std::function<void()> job, std::function<void()> done) override
    {
        if (job)
            job();
        if (done)
            done();
    }
};

// Any object with run(std::function<void()>, std::function<void()>) (services::library::Workers).
template <typename Workers>
class WorkerPoolAdapter final : public WorkerPool
{
public:
    explicit WorkerPoolAdapter(Workers& w) noexcept : workers_(w) {}
    void run(std::function<void()> job, std::function<void()> done) override
    {
        workers_.run(std::move(job), std::move(done));
    }

private:
    Workers& workers_;
};
} // namespace core::edit
