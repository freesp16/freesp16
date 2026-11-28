// core::PiMutex: a working mutex everywhere, with priority inheritance where the kernel has PI futexes.
#include "core/util/PiMutex.h"

#include <catch2/catch_test_macros.hpp>

#include <mutex>
#include <thread>
#include <vector>

TEST_CASE("PiMutex excludes and works with lock_guard", "[threads]")
{
    core::PiMutex m;
    long counter = 0;
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i)
        threads.emplace_back(
            [&]
            {
                for (int n = 0; n < 50000; ++n)
                {
                    std::lock_guard<core::PiMutex> lock(m);
                    ++counter;
                }
            });
    for (auto& t : threads)
        t.join();
    CHECK(counter == 200000);
    REQUIRE(m.try_lock());
    m.unlock();
}

TEST_CASE("PiMutex: try_lock fails while another thread holds it", "[threads]")
{
    core::PiMutex m;
    m.lock();
    bool got = true;
    std::thread([&] { got = m.try_lock(); }).join();
    CHECK_FALSE(got);
    m.unlock();
    std::thread(
        [&]
        {
            got = m.try_lock();
            if (got)
                m.unlock();
        })
        .join();
    CHECK(got);
}

#if defined(__linux__)
TEST_CASE("PiMutex: a Linux kernel offers priority inheritance", "[threads]")
{
    CHECK(core::PiMutex::inheritanceAvailable());
}
#endif
