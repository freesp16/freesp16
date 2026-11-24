// app::tuning: the code lock and the governor report what they did; nothing here needs a right the test may lack.
#include "app/SystemTuning.cpp"

#include <catch2/catch_test_macros.hpp>

#include <sys/mman.h>

TEST_CASE("system tuning: the code lock visits the executable's mappings and reports them", "[app][tuning]")
{
    const auto r = app::tuning::lockCode();
#if defined(__linux__)
    CHECK(r.mappings + r.failed > 0); // the test binary's own code at least
    if (r.failed == 0)
        CHECK(r.lockedKb > 0);
    ::munlockall(); // leave the test process as it was
#endif
    const auto line = app::tuning::describe(r);
    CHECK(line.rfind("memlock code locked_kb=", 0) == 0);
    CHECK(line.find(" swap_kb=") != std::string::npos);
}

TEST_CASE("system tuning: an empty governor name changes nothing", "[app][tuning]")
{
    const auto g = app::tuning::setGovernor("");
    CHECK_FALSE(g.changed);
    CHECK(g.written == 0);
    CHECK(g.previous.empty());
    app::tuning::restore(g);
    const auto line = app::tuning::describe(g, "");
    CHECK(line.find("wanted=keep") != std::string::npos);
}

TEST_CASE("system tuning: an empty real-time limit changes nothing", "[app][tuning]")
{
    const auto r = app::tuning::setRtRuntime("");
    CHECK_FALSE(r.changed);
    app::tuning::restore(r);
    CHECK(app::tuning::describe(r, "").find("rt_runtime_us before=") == 0);
}
