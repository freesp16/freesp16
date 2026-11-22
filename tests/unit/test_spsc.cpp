#include "core/util/SpscQueue.h"

#include <catch2/catch_test_macros.hpp>

#include <thread>

TEST_CASE("SPSC queue keeps order and reports full", "[spsc]")
{
    core::SpscQueue<int, 4> q;
    for (int i = 0; i < 4; ++i)
        CHECK(q.push(i));
    CHECK_FALSE(q.push(99));
    int v = -1;
    for (int i = 0; i < 4; ++i)
    {
        REQUIRE(q.pop(v));
        CHECK(v == i);
    }
    CHECK_FALSE(q.pop(v));
}

TEST_CASE("SPSC queue across two threads", "[spsc]")
{
    core::SpscQueue<std::uint32_t, 256> q;
    constexpr std::uint32_t count = 200000;
    std::thread producer([&] {
        for (std::uint32_t i = 0; i < count;)
            if (q.push(i))
                ++i;
    });
    std::uint32_t expected = 0, v = 0;
    bool inOrder = true;
    while (expected < count)
        if (q.pop(v))
            inOrder &= (v == expected++);
    producer.join();
    CHECK(inOrder);
}
