// Lock-free channels between threads (threading.md "Channels between threads", NFR-020/021): TripleBuffer (audio → UI
// snapshots), MpscQueue (reader threads → audio, UI-118), RetireQueue (object swap: never free on the audio thread),
// and the static registrar behind every APP_REGISTER_* macro.
#include "core/util/MpscQueue.h"
#include "core/util/Realtime.h"
#include "core/util/Registrar.h"
#include "core/util/RetireQueue.h"
#include "core/util/Result.h"
#include "core/util/TripleBuffer.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

namespace
{
int registrarRuns = 0;
void countRegistration(int n) { registrarRuns += n; }
} // namespace

APP_STATIC_REGISTRAR(countRegistration(1))
APP_STATIC_REGISTRAR(countRegistration(2))

TEST_CASE("static registrars run before main", "[contracts][util]") { CHECK(registrarRuns == 3); }

TEST_CASE("TripleBuffer: newest snapshot wins, reader never sees a torn one", "[contracts][util]")
{
    struct Snap
    {
        std::uint64_t a = 0, b = 0;
    };
    core::TripleBuffer<Snap> tb;
    CHECK(tb.read().a == 0);
    CHECK_FALSE(tb.fresh());
    tb.write() = {1, 1};
    tb.publish();
    tb.write() = {2, 2};
    tb.publish();
    CHECK(tb.fresh());
    CHECK(tb.read().a == 2);
    CHECK(tb.read().a == 2); // stable without a new publish
    CHECK_FALSE(tb.fresh());

    std::atomic<bool> done {false};
    std::thread writer([&] {
        core::rt::ScopedRealtimeThread realtime;
        for (std::uint64_t i = 3; i < 200000; ++i)
        {
            auto& s = tb.write();
            s.a = i;
            s.b = i;
            tb.publish();
        }
        done = true;
    });
    std::uint64_t last = 0;
    bool ordered = true, consistent = true;
    while (! done)
    {
        const auto& s = tb.read();
        consistent = consistent && s.a == s.b;
        ordered = ordered && s.a >= last;
        last = s.a;
    }
    writer.join();
    CHECK(consistent);
    CHECK(ordered);
    CHECK(tb.read().a == 199999);
}

TEST_CASE("MpscQueue: several producers, one consumer, nothing lost or duplicated", "[contracts][util]")
{
    core::MpscQueue<std::uint32_t, 1024> q;
    constexpr int producers = 4, each = 20000;
    std::vector<std::thread> threads;
    for (int p = 0; p < producers; ++p)
        threads.emplace_back([&q, p] {
            for (int i = 0; i < each; ++i)
                while (! q.push(static_cast<std::uint32_t>(p * each + i)))
                    std::this_thread::yield();
        });
    std::vector<int> seen(producers * each, 0);
    int received = 0;
    {
        core::rt::ScopedRealtimeThread realtime; // the consumer side is the audio thread
        std::uint32_t v;
        while (received < producers * each)
            if (q.pop(v))
            {
                ++seen[v];
                ++received;
            }
    }
    for (auto& t : threads)
        t.join();
    CHECK(std::all_of(seen.begin(), seen.end(), [](int n) { return n == 1; }));
    std::uint32_t v;
    CHECK_FALSE(q.pop(v));
}

TEST_CASE("RetireQueue: freed off the audio thread after the grace period", "[contracts][util]")
{
    struct Tracked
    {
        explicit Tracked(int& c) : count(c) { ++count; }
        ~Tracked() { --count; }
        int& count;
    };
    int alive = 0;
    core::RetireQueue<8> q;
    auto* a = new Tracked(alive);
    auto* b = new Tracked(alive);
    {
        core::rt::ScopedRealtimeThread realtime; // retiring must not free (the hook would abort)
        CHECK(q.retireObject(a, 10));
        CHECK(q.retireObject(b, 11));
    }
    CHECK(alive == 2);
    CHECK(q.collect(11) == 0); // a is 1 block old
    CHECK(q.pending() == 2);
    CHECK(q.collect(12) == 1); // a: 2 blocks
    CHECK(alive == 1);
    CHECK(q.collect(100) == 1);
    CHECK(alive == 0);
    CHECK(q.pending() == 0);
}

TEST_CASE("Result carries a value or an error", "[contracts][util]")
{
    core::Result<int> ok = 3;
    CHECK(ok.ok());
    CHECK(ok.value() == 3);
    auto bad = core::Result<int>::failure("cannot read", "detail");
    CHECK_FALSE(bad);
    CHECK(bad.error().message == "cannot read");
    core::Result<void> done;
    CHECK(done.ok());
}
