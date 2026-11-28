// app::UiThrottle: the display steps down to every second frame under audio load and back up after the hold.
#include "app/UiThrottle.h"

#include <catch2/catch_test_macros.hpp>

using app::UiThrottle;

namespace
{
int drawn(UiThrottle& t, double& now, int frames)
{
    int n = 0;
    for (int i = 0; i < frames; ++i, now += 1000.0 / 60.0)
        n += t.draw(now, nullptr) ? 1 : 0;
    return n;
}
} // namespace

TEST_CASE("ui throttle: the first sample is the baseline, start-up xruns do not count", "[app][throttle]")
{
    UiThrottle t;
    double now = 0;
    UiThrottle::Sample s {0.1, 5, 2};
    CHECK(t.draw(now, &s));
    CHECK_FALSE(t.throttled());
    CHECK(t.takeChange() == UiThrottle::Change::None);
    CHECK(drawn(t, now, 60) == 60);
}

TEST_CASE("ui throttle: an xrun halves the frame rate until the hold has passed", "[app][throttle]")
{
    UiThrottle t(0.8, 3000.0);
    double now = 0;
    UiThrottle::Sample s {0.1, 0, 0};
    t.draw(now, &s);
    s.xruns = 2;
    now += 250;
    t.draw(now, &s);
    CHECK(t.throttled());
    CHECK(t.reason() == "xrun +2");
    CHECK(t.takeChange() == UiThrottle::Change::On);
    CHECK(t.takeChange() == UiThrottle::Change::None);
    CHECK(drawn(t, now, 60) == 30); // 30 Hz
    // Quiet samples: it stays down until 3 s after the xrun, then draws every frame again.
    while (now < 250 + 3000 + 20)
    {
        t.draw(now, &s);
        now += 1000.0 / 60.0;
    }
    CHECK_FALSE(t.throttled());
    CHECK(t.takeChange() == UiThrottle::Change::Off);
    CHECK(drawn(t, now, 60) == 60);
}

TEST_CASE("ui throttle: engine overruns and a high p95 load trigger it too, and extend the hold", "[app][throttle]")
{
    UiThrottle t(0.8, 1000.0);
    double now = 0;
    UiThrottle::Sample s {0.5, 0, 0};
    t.draw(now, &s);
    s.overruns = 1;
    t.draw(now += 250, &s);
    CHECK(t.reason() == "overrun +1");
    s.p95Fraction = 0.83;
    for (int i = 0; i < 8; ++i) // sustained load: 2 s of samples keep it down
        t.draw(now += 250, &s);
    CHECK(t.throttled());
    CHECK(t.reason() == "overrun +1"); // the cause of the step down
    s.p95Fraction = 0.4;
    t.draw(now += 250, &s);
    CHECK(t.throttled());
    t.draw(now += 1000, &s);
    CHECK_FALSE(t.throttled());
    UiThrottle load(0.8, 1000.0);
    UiThrottle::Sample high {0.81, 0, 0};
    load.draw(0, &high);
    load.draw(250, &high);
    CHECK(load.throttled());
    CHECK(load.reason() == "load p95 81%");
}
