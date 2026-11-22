// No allocation after warm-up: a full frame build and the hot input events
// (encoders, selector, pads, SHIFT, step hold) run with the real-time allocation hook armed (APP_RT_CHECKS, host
// preset). Opening screens and popups may allocate; showing them afterwards may not.
#include "core/util/Realtime.h"
#include "tests/unit/ui/ShellHarness.h"
#include "ui/shell/Layout.h"

#include <catch2/catch_test_macros.hpp>

namespace
{
struct Counting
{
    std::size_t before = core::rt::violationCount();
    Counting() { core::rt::setAbortOnViolation(false); }
    ~Counting() { core::rt::setAbortOnViolation(true); }
    std::size_t count() const { return core::rt::violationCount() - before; }
};
} // namespace

TEST_CASE("shell alloc: frame build and hot input allocate nothing after warm-up", "[ui][shell][alloc]")
{
    uitest::Harness h;
    h.shell = nullptr; // rebuild without the log sink (log lines are strings: the app's trace choice, not the frame's)
    ui::ShellConfig cfg{core::builtinParams(), uitest::fixedMeasure()};
    cfg.screens = &h.screens;
    h.shell = std::make_unique<ui::Shell>(std::move(cfg));
    for (int i = 0; i < 3; ++i)
        h.frame(16);
    // A popup and a toast stay open while measuring: drawing them must not allocate either.
    ui::ListRequest r;
    r.title = "LIST";
    r.items = {{"A", {}, {}, 0}, {"B", {}, {}, 1}};
    h.shell->showList(r);
    ui::ToastRequest toast;
    toast.text = "Saved";
    h.shell->showToast(toast);
    h.frame(16);
    h.press(io::Control::Back); // close the list again (closing may free: outside the measured window)
    h.frame(16);
    h.frame(200); // the close fade (motion.base) ends and frees the popup
    h.shell->showList(r);
    h.frame(16);

    Counting counting;
    {
        core::rt::ScopedRealtimeThread rt;
        for (int i = 0; i < 20; ++i)
        {
            h.turn(io::knob(1 + i % 6), i % 2 ? 1 : -1);
            h.turn(io::Control::Selector, 1);
            h.press(io::pad(1 + i % 16));
            h.press(io::Control::Shift);
            h.press(io::step(1 + i % 16));
            h.turn(io::knob(3), 2);
            h.release(io::step(1 + i % 16));
            h.release(io::Control::Shift);
            h.frame(16);
        }
    }
    CHECK(counting.count() == 0);
}

TEST_CASE("shell alloc: the hook sees an allocation (the test above is not vacuous)", "[ui][shell][alloc]")
{
    Counting counting;
    {
        core::rt::ScopedRealtimeThread rt;
        int* volatile p = new int(1); // volatile: the optimiser may not elide the pair in Release builds
        delete p;
    }
    CHECK(counting.count() > 0);
}
