// Preview feed (LIB-011, UID-677): the focus settles 120 ms before a decode starts, a new focus cancels the old one,
// PREVIEW off is silent and switching it on plays the focused file once, the engine gets ObjectSwap{Preview} and
// preview buffers are freed once the engine lets go.
#include "LibTestSupport.h"

#include "services/library/PreviewService.h"

#include <catch2/catch_test_macros.hpp>

using namespace services::library;

TEST_CASE("lib.preview: debounced focus, cancel on refocus, null swap on stop", "[library][preview]")
{
    libtest::Fixture f;
    const auto a = libtest::writeSample(f.samples().getChildFile("a.wav"), 44100 * 40, 100); // 40 s: 30 s decoded
    const auto b = libtest::writeSample(f.samples().getChildFile("b.wav"), 2000, 200);
    libtest::SwapEngine engine;
    double now = 0;
    {
        PreviewService preview(f.pool, f.workers, engine, [&] { return now; });
        preview.focus(a);
        now = 100;
        preview.tick();
        f.workers.drain();
        CHECK(engine.swaps.empty()); // still settling

        preview.focus(b); // fast scrolling: a never plays
        now = 150;
        preview.tick();
        now = 210;
        preview.tick();
        f.workers.drain();
        CHECK(engine.swaps.empty());
        now = 221;
        preview.tick();
        f.workers.drain();
        REQUIRE(engine.swaps.size() == 1);
        CHECK(engine.swaps[0].slot.kind == engine::SwapSlot::Preview);
        REQUIRE(engine.preview() != nullptr);
        CHECK(engine.preview()->frames == 2000);
        CHECK(preview.playing() == b);

        preview.focus(a);
        now = 500;
        preview.tick();
        f.workers.drain();
        REQUIRE(engine.preview() != nullptr);
        CHECK(engine.preview()->frames == 44100 * 30); // first 30 s only
        preview.tick();
        CHECK(preview.ownedBuffers() == 1); // b's buffer was released by the engine and freed

        preview.stop();
        CHECK(engine.preview() == nullptr);
        CHECK(engine.swaps.back().object == nullptr);
        preview.tick();
        CHECK(preview.ownedBuffers() == 0);
    }
}

TEST_CASE("lib.preview: PREVIEW off is silent, switching it on plays the focused file at once", "[library][preview]")
{
    libtest::Fixture f;
    const auto a = libtest::writeSample(f.samples().getChildFile("a.wav"), 1000, 100);
    libtest::SwapEngine engine;
    double now = 0;
    PreviewService preview(f.pool, f.workers, engine, [&] { return now; });
    preview.setEnabled(false);
    preview.focus(a);
    now = 1000;
    preview.tick();
    f.workers.drain();
    CHECK(engine.preview() == nullptr);
    preview.setEnabled(true); // no debounce
    f.workers.drain();
    CHECK(engine.preview() != nullptr);

    // A resident file previews from the pool (no decode, no own buffer).
    SamplePool::Handle pooled = nullptr;
    const auto c = libtest::writeSample(f.samples().getChildFile("c.wav"), 3000, 100);
    f.pool.requestFile(c, false, [&](core::Result<SamplePool::Handle> r, const LoadInfo&) { pooled = r.value(); });
    f.workers.drain();
    now = 2000;
    preview.focus(c);
    now = 2200;
    preview.tick();
    CHECK(engine.preview() == pooled);
    preview.tick();
    CHECK(preview.ownedBuffers() == 0);

    // Focus on a folder stops; an undecodable file reports and stays silent.
    preview.focus(f.samples());
    CHECK(engine.preview() == nullptr);
    std::string error;
    preview.onError = [&](const core::Error& e) { error = e.message; };
    const juce::File bad = f.samples().getChildFile("bad.wav");
    REQUIRE(bad.replaceWithText("nope nope nope"));
    now = 3000;
    preview.focus(bad);
    now = 3200;
    preview.tick();
    f.workers.drain();
    CHECK_FALSE(error.empty());
    CHECK(engine.preview() == nullptr);
}
