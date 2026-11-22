// Asynchronous pastes (EDT-004): the worker sees only the clipboard snapshot, the model changes when the command is
// installed (one undo step), pastes install in submission order, the busy indicator appears after 100 ms, the
// clipboard is single and overwritten (EDT-010), a kind mismatch cannot paste (EDT-011), and a paste whose project was
// replaced or whose editor is gone is dropped. Also a real worker thread.
#include "tests/unit/edit/EditTestUtil.h"

#include "core/edit/SceneEditor.h"
#include "core/edit/SharedEditor.h"
#include "tests/fakes/FakeClock.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace ce = core::edit;
namespace m = core::model;

TEST_CASE("edit.async: the model changes only when the prepared paste is installed, one undo step", "[edit]")
{
    edittest::Bus b;
    edittest::ManualPool pool;
    fakes::FakeClock clock(1000.0);
    ce::SceneEditor editor(b.bus, pool, &clock);
    const m::Scene source = b.project().scenes[1];
    editor.copy(ce::sceneRef(1));
    b.project().scenes[1].name = "changed after COPY"; // the clipboard holds the state at COPY time
    int results = 0;
    editor.paste(ce::sceneRef(5), [&results](ce::Result r) { results += r.ok() ? 1 : 100; });
    CHECK(editor.busy());
    CHECK_FALSE(b.project().scenes[5] == source);
    CHECK_FALSE(editor.busyIndicator());
    clock.advance(99.0);
    CHECK_FALSE(editor.busyIndicator());
    clock.advance(2.0);
    CHECK(editor.busyIndicator()); // > 100 ms (EDT-004)
    CHECK(b.undoDepth() == 0);
    pool.finish();
    CHECK(results == 1);
    CHECK_FALSE(editor.busy());
    CHECK_FALSE(editor.busyIndicator());
    CHECK(b.project().scenes[5] == source);
    CHECK(b.undoDepth() == 1);
}

TEST_CASE("edit.async: pastes install in the order they were made, whatever order the workers finish in", "[edit]")
{
    edittest::Bus b;
    edittest::ManualPool pool;
    ce::SceneEditor editor(b.bus, pool);
    const m::Pattern p1 = b.pattern(0, 1), p2 = b.pattern(0, 2);
    std::vector<int> order;
    editor.copy(ce::patternRef(0, 1));
    editor.paste(ce::patternRef(3, 0), [&order](ce::Result) { order.push_back(1); });
    editor.copy(ce::patternRef(0, 2));
    editor.paste(ce::patternRef(3, 0), [&order](ce::Result) { order.push_back(2); });
    CHECK(editor.pending() == 2);
    pool.finish(1); // the second worker finishes first: it waits for the first
    CHECK(order.empty());
    CHECK(editor.pending() == 2);
    pool.finish(0);
    CHECK(order == std::vector<int> {1, 2});
    CHECK(b.pattern(3, 0) == p2); // the later paste wins
    CHECK(b.undoDepth() == 2);
    REQUIRE(b.bus.undo());
    CHECK(b.pattern(3, 0) == p1);
}

TEST_CASE("edit.async: one clipboard, overwritten by a second COPY, PASTE needs the same kind (EDT-010/011)", "[edit]")
{
    edittest::Bus b;
    ce::InlineWorkerPool pool;
    ce::SceneEditor editor(b.bus, pool);
    CHECK(editor.clipboard().empty());
    CHECK_FALSE(editor.canPaste(ce::sceneRef(0)));
    CHECK_FALSE(editor.canPaste(ce::patternRef(0, 0)));
    editor.copy(ce::sceneRef(2));
    CHECK(editor.isCopied(ce::sceneRef(2)));
    CHECK_FALSE(editor.isCopied(ce::sceneRef(3)));
    CHECK(editor.canPaste(ce::sceneRef(0)));
    CHECK_FALSE(editor.canPaste(ce::patternRef(0, 0))); // a scene on the clipboard, a pattern focused
    editor.copy(ce::patternRef(1, 4));
    CHECK_FALSE(editor.isCopied(ce::sceneRef(2))); // overwritten
    CHECK(editor.isCopied(ce::patternRef(1, 4)));
    CHECK_FALSE(editor.canPaste(ce::sceneRef(0)));
    CHECK(editor.canPaste(ce::patternRef(0, 0)));
    // Families: a track with sequences pastes onto a track focus; steps onto a page.
    editor.copy(ce::trackRef(0, 1, true));
    CHECK(editor.canPaste(ce::trackRef(0, 2, false)));
    CHECK_FALSE(editor.canPaste(ce::sequenceRef(0, 0, 2)));
    editor.copy(ce::stepsRef(0, 0, 1, 0b1011));
    CHECK(editor.canPaste(ce::pageRef(0, 1, 1, 2)));
    // A mismatch reports a failure and changes nothing.
    const m::Project before = b.project();
    bool failed = false;
    editor.paste(ce::sceneRef(4), [&failed](ce::Result r) { failed = ! r.ok(); });
    CHECK(failed);
    CHECK(b.project() == before);
    CHECK(b.undoDepth() == 0);
}

TEST_CASE("edit.async: a project replaced while a paste is prepared drops it, a destroyed editor ignores it", "[edit]")
{
    edittest::Bus b;
    edittest::ManualPool pool;
    {
        ce::SceneEditor editor(b.bus, pool);
        editor.copy(ce::patternRef(0, 1));
        bool failed = false;
        editor.paste(ce::patternRef(0, 2), [&failed](ce::Result r) { failed = ! r.ok(); });
        core::commands::replaceProject(b.bus, m::newDefaultProject()); // project open
        pool.finish();
        CHECK(failed);
        CHECK(b.pattern(0, 2) == m::defaultPattern());
        CHECK(b.undoDepth() == 0);
        // Track and scene clips belong to their project (sample references): no paste into another one.
        CHECK(editor.canPaste(ce::patternRef(0, 3))); // pattern clips do paste across projects
    }
    {
        auto editor = std::make_unique<ce::SceneEditor>(b.bus, pool);
        editor->copy(ce::patternRef(0, 0));
        bool called = false;
        editor->paste(ce::patternRef(0, 5), [&called](ce::Result) { called = true; });
        editor.reset();
        pool.finish(); // the job ran on its own data; the completion finds the editor gone
        CHECK_FALSE(called);
        CHECK(b.undoDepth() == 0);
    }
    edittest::Bus c;
    ce::InlineWorkerPool inl;
    ce::SceneEditor e2(c.bus, inl);
    e2.copy(ce::trackRef(0, 0, false));
    CHECK(e2.canPaste(ce::trackRef(0, 1, false)));
    core::commands::replaceProject(c.bus, m::newDefaultProject());
    CHECK_FALSE(e2.canPaste(ce::trackRef(0, 1, false)));
}

namespace
{
// A real worker thread with a completion queue the test pumps (the shape of services::library::Workers).
class ThreadPool final : public ce::WorkerPool
{
public:
    ThreadPool() : thread_([this] { loop(); }) {}
    ~ThreadPool() override
    {
        {
            std::lock_guard<std::mutex> l(m_);
            stop_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }
    void run(std::function<void()> job, std::function<void()> done) override
    {
        {
            std::lock_guard<std::mutex> l(m_);
            jobs_.emplace_back(std::move(job), std::move(done));
        }
        cv_.notify_all();
    }
    // Message thread: runs the finished completions; returns how many.
    int pump()
    {
        std::vector<std::function<void()>> done;
        {
            std::lock_guard<std::mutex> l(m_);
            done.swap(done_);
        }
        for (auto& d : done)
            d();
        return static_cast<int>(done.size());
    }
    std::atomic<std::thread::id> workerId {};

private:
    void loop()
    {
        workerId = std::this_thread::get_id();
        std::unique_lock<std::mutex> l(m_);
        for (;;)
        {
            cv_.wait(l, [this] { return stop_ || ! jobs_.empty(); });
            if (jobs_.empty())
                return;
            auto [job, done] = std::move(jobs_.front());
            jobs_.erase(jobs_.begin());
            l.unlock();
            job();
            l.lock();
            done_.push_back(std::move(done));
        }
    }
    std::mutex m_;
    std::condition_variable cv_;
    std::vector<std::pair<std::function<void()>, std::function<void()>>> jobs_;
    std::vector<std::function<void()>> done_;
    bool stop_ = false;
    std::thread thread_;
};
} // namespace

TEST_CASE("edit.async: a full scene prepared on a real worker thread lands as one undoable step", "[edit]")
{
    edittest::Bus b;
    ThreadPool pool;
    ce::SceneEditor editor(b.bus, pool);
    const m::Project before = b.project();
    editor.copy(ce::sceneRef(0));
    bool ok = false;
    editor.paste(ce::sceneRef(9), [&ok](ce::Result r) { ok = r.ok(); });
    // Meanwhile the message thread keeps editing the model (the worker never reads it).
    b.bus.execute(core::edit::makeReset(ce::patternRef(1, 1)));
    for (int i = 0; i < 2000 && editor.busy(); ++i)
    {
        pool.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE_FALSE(editor.busy());
    CHECK(ok);
    CHECK(b.project().scenes[9] == before.scenes[0]);
    CHECK(b.undoDepth() == 2);
    REQUIRE(b.bus.undo());
    CHECK(b.project().scenes[9] == before.scenes[9]);
}

TEST_CASE("edit.async: the shared editor keeps one clipboard across a bus change and runs inline by default", "[edit]")
{
    ce::resetSharedEditor();
    edittest::Bus a, b;
    auto& ea = ce::sharedEditor(a.bus);
    ea.copy(ce::patternRef(0, 1));
    auto& eb = ce::sharedEditor(b.bus);
    CHECK(eb.isCopied(ce::patternRef(0, 1)));
    eb.paste(ce::patternRef(0, 2)); // inline: done at once
    CHECK(b.pattern(0, 2) == a.pattern(0, 1));
    CHECK_FALSE(eb.busy());
    ce::resetSharedEditor();
}
