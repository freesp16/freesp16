// Damage tracking: the tracker merges rects; the frame diff finds exactly the commands that
// changed (also when commands are inserted or removed); a partial redraw is pixel-identical to a full redraw of the
// same frame for both CPU backends, over random edit sequences.
#include "tests/unit/render/RenderFixtures.h"
#include "ui/render/Damage.h"
#include "ui/render/GlyphAtlas.h"
#include "ui/render/OffscreenRenderer.h"
#include "ui/render/Renderer.h"

#include <juce_events/juce_events.h>

#include <catch2/catch_test_macros.hpp>

#include <random>

namespace
{
bool contains(std::span<const ui::RectF> rects, ui::RectF r)
{
    for (const auto& d : rects)
        if (d.x <= r.x && d.y <= r.y && d.right() >= r.right() && d.bottom() >= r.bottom())
            return true;
    return false;
}

// A frame with `n` tiles; tile `changed` gets another colour, tile `extra` an extra overlay (inserted command).
void tiles(ui::DrawList& dl, int changed = -1, int extra = -1, int skip = -1)
{
    dl.clear();
    dl.fill({0, 0, 1280, 768}, 0xFF0B0D10u);
    for (int i = 0; i < 40; ++i)
    {
        if (i == skip)
            continue;
        const ui::RectF r{20.0f + 150.0f * (i % 8), 20.0f + 140.0f * static_cast<float>(i / 8), 120, 100};
        dl.fill(r, i == changed ? 0xFFFF0000u : 0xFF1A1E25u, 8);
        if (i == extra)
            dl.fill({r.x + 10, r.y + 10, 30, 30}, 0x80FFFFFFu, 4);
        dl.text(r.x + 60, r.y + 50, ui::Align::Centre, ui::TextStyleId::Label, 0xFFE8ECF2u, "TILE");
    }
}
} // namespace

TEST_CASE("render damage: tracker merges near rects, caps the count and reports full", "[render][damage]")
{
    ui::DamageTracker t;
    CHECK(t.empty());
    t.invalidate({10, 10, 20, 20});
    t.invalidate({32, 10, 20, 20}); // 2 lpx apart: merged
    REQUIRE(t.rects().size() == 1);
    CHECK(t.rects()[0].x == 10);
    CHECK(t.rects()[0].right() == 52);
    for (int i = 0; i < 20; ++i)
        t.invalidate({100.0f + 50.0f * i, 300, 10, 10});
    CHECK(t.rects().size() <= static_cast<std::size_t>(ui::DamageTracker::kMaxRects));
    for (int i = 0; i < 20; ++i)
        CHECK(contains(t.rects(), {100.0f + 50.0f * i, 300, 10, 10}));
    t.invalidate({-50, -50, 10, 10}); // off canvas: ignored
    auto taken = t.take();
    CHECK(! taken.empty());
    CHECK(t.empty());
    t.invalidateAll();
    taken = t.take();
    REQUIRE(taken.size() == 1);
    CHECK(taken[0].w == 1280);
    CHECK(taken[0].h == 768);
}

TEST_CASE("render damage: the frame diff damages only changed, inserted and removed commands", "[render][damage]")
{
    ui::FrameDiff diff;
    ui::DamageTracker t;
    ui::DrawList a;
    tiles(a);
    diff.diff(a, t);
    CHECK(t.full()); // first frame
    t.take();
    diff.diff(a, t);
    CHECK(t.empty()); // identical frame

    ui::DrawList b;
    tiles(b, 12);
    diff.diff(b, t);
    REQUIRE(t.rects().size() == 1);
    const ui::RectF tile12{20.0f + 150.0f * 4, 20.0f + 140.0f, 120, 100};
    CHECK(contains(t.rects(), tile12));
    CHECK(t.rects()[0].w < 140); // only that tile (+ margins)
    t.take();

    tiles(b, 12, 30); // insertion in tile 30: no cascade over the following commands
    diff.diff(b, t);
    REQUIRE(t.rects().size() == 1);
    CHECK(t.rects()[0].w < 60);
    t.take();

    tiles(b, 12, -1, 5); // tile 5 removed, tile 30's overlay removed
    diff.diff(b, t);
    CHECK(t.rects().size() == 2);
    t.take();

    // The same frame again: nothing.
    tiles(b, 12, -1, 5);
    diff.diff(b, t);
    CHECK(t.empty());
}

TEST_CASE("render damage: partial redraws are pixel-identical to full redraws (soft and juce)", "[render][damage]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ui::GlyphAtlas atlas;
    std::string error;
    REQUIRE(atlas.build(0.625f, error));
    for (const bool juceBackend : {false, true})
    {
        INFO((juceBackend ? "juce" : "soft"));
        auto raster = [&] { return juceBackend ? ui::makeJuceRasterizer(nullptr, 0) : ui::makeSoftRasterizer(atlas); };
        auto damaged = ui::makeCpuRenderer(raster(), ui::makeMemoryPresenter(800, 480), &atlas);
        auto full = ui::makeCpuRenderer(raster(), ui::makeMemoryPresenter(800, 480), &atlas);
        full->setDamageTracking(false);
        std::mt19937 rng(1234);
        long partialFrames = 0, skipped = 0;
        for (int frame = 0; frame < (juceBackend ? 30 : 120); ++frame)
        {
            ui::DrawList dl;
            const int pick = static_cast<int>(rng() % 6);
            if (pick == 0)
                rendertest::referenceList(dl, static_cast<int>(rng() % 5));
            else
                tiles(dl, static_cast<int>(rng() % 48) - 4, static_cast<int>(rng() % 48) - 4,
                      static_cast<int>(rng() % 60) - 10);
            if (rng() % 7 == 0)
                dl.invalidate({static_cast<float>(rng() % 1200), static_cast<float>(rng() % 700), 50, 40});
            ui::FrameTiming t1, t2;
            REQUIRE(damaged->render(dl, t1));
            REQUIRE(full->render(dl, t2));
            partialFrames += t1.damagedPixels > 0 && t1.damagedPixels < 800L * 480;
            skipped += t1.damagedPixels == 0;
            std::vector<std::uint32_t> a, b;
            REQUIRE(damaged->readPixels(a));
            REQUIRE(full->readPixels(b));
            INFO("frame " << frame);
            REQUIRE(rendertest::rgbOnly(a) == rendertest::rgbOnly(b));
        }
        CHECK(partialFrames > 10);
        CHECK(skipped > 0);
    }
}

TEST_CASE("render damage: OffscreenRenderer skips unchanged frames and redraws the changed rect only",
          "[render][damage]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ui::render::OffscreenRenderer off;
    std::string error;
    REQUIRE(off.open(800, 480, error));
    ui::DrawList dl;
    tiles(dl);
    ui::FrameTiming t;
    const auto first = off.draw(dl, &t);
    CHECK(t.damagedPixels == 800L * 480);
    off.draw(dl, &t);
    CHECK(t.damagedPixels == 0);
    tiles(dl, 3);
    const auto second = off.draw(dl, &t);
    CHECK(t.damagedPixels > 0);
    CHECK(t.damagedPixels < 800L * 480 / 20);
    CHECK(first != second);
    CHECK(off.measure()(ui::TextStyleId::Label, "TILE") > 10.0f);
}
