// Renderer contract: every backend that can run on this
// host — the registered ones without a child probe (the GL backends need the device's driver) — draws the reference
// DrawList within the snapshot tolerance of `soft`, at the panel size and at the canvas size, with damage tracking on
// and off, and a second identical frame changes nothing.
#include "tests/unit/render/RenderFixtures.h"
#include "ui/render/GlyphAtlas.h"
#include "ui/render/RendererRegistry.h"

#include <juce_events/juce_events.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>

namespace
{
ui::RendererEnv hostEnv(const ui::GlyphAtlas& atlas, int w, int h)
{
    ui::RendererEnv env;
    env.width = w;
    env.height = h;
    env.atlas = &atlas;
    env.windowPresenter = [w, h](std::string&) { return ui::makeMemoryPresenter(w, h); };
    return env;
}

std::vector<std::uint32_t> drawWith(ui::Renderer& r, const ui::DrawList& dl)
{
    ui::FrameTiming t;
    REQUIRE(r.render(dl, t));
    std::vector<std::uint32_t> px;
    REQUIRE(r.readPixels(px));
    return rendertest::rgbOnly(px);
}
} // namespace

TEST_CASE("renderer contract: every host backend draws the reference list within tolerance of soft",
          "[render][contract]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ui::DrawList dl;
    rendertest::referenceList(dl);
    for (const auto [w, h] : {std::pair{800, 480}, std::pair{1280, 768}, std::pair{1024, 600}})
    {
        ui::GlyphAtlas atlas;
        std::string error;
        REQUIRE(atlas.build(ui::CanvasTransform::fit(w, h).scale, error));
        const auto env = hostEnv(atlas, w, h);
        auto soft = ui::render::create("soft", env, error);
        REQUIRE(soft);
        const auto reference = drawWith(*soft, dl);
        int backends = 0;
        for (const auto& entry : ui::render::registeredRenderers())
        {
            if (entry.needsChildProbe || entry.name.rfind("contract", 0) == 0)
                continue;
            INFO("backend " << entry.name << " at " << w << "x" << h);
            auto r = ui::render::create(entry.name, env, error);
            REQUIRE(r);
            CHECK(r->width() == w);
            CHECK(r->height() == h);
            const auto px = drawWith(*r, dl);
            const auto c = rendertest::compare(px, reference, w, h);
            std::printf("RENDER contract %.*s %dx%d: %.2f %% within tolerance of soft (max channel diff %d)\n",
                        static_cast<int>(entry.name.size()), entry.name.data(), w, h, c.match * 100.0, c.maxDiff);
            CHECK(c.match >= 0.97);
            // Unchanged frame: nothing redrawn, pixels identical.
            ui::FrameTiming t;
            REQUIRE(r->render(dl, t));
            CHECK(t.damagedPixels == 0);
            // Damage tracking off: every frame in full, same pixels.
            r->setDamageTracking(false);
            REQUIRE(r->render(dl, t));
            CHECK(t.damagedPixels == static_cast<long>(w) * h);
            std::vector<std::uint32_t> again;
            REQUIRE(r->readPixels(again));
            CHECK(rendertest::rgbOnly(again) == px);
            ++backends;
        }
        CHECK(backends >= 2); // soft and juce
    }
}

TEST_CASE("renderer contract: the registry falls back in order and reports why", "[render][contract]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ui::GlyphAtlas atlas;
    std::string error;
    REQUIRE(atlas.build(0.625f, error));
    auto env = hostEnv(atlas, 800, 480);
    const std::vector<std::string> order{"no-such-backend", "soft", "juce"};
    std::string log;
    auto r = ui::render::createFirstWorking(order, env, log);
    REQUIRE(r);
    CHECK(r->name() == "soft");
    CHECK(log.find("no-such-backend unavailable") != std::string::npos);
    CHECK(log.find("soft selected") != std::string::npos);

    env.windowPresenter = nullptr; // no presenter: every CPU backend fails, the list is exhausted
    log.clear();
    CHECK(ui::render::createFirstWorking(order, env, log) == nullptr);
    CHECK(log.find("soft unavailable: no presenter") != std::string::npos);
}

TEST_CASE("renderer contract: a crashing backend is ruled out by its child probe", "[render][contract]")
{
    struct Crash
    {
        static std::unique_ptr<ui::Renderer> make(const ui::RendererEnv&, std::string&)
        {
            std::abort(); // a driver crash in the child
        }
    };
    ui::render::registerRenderer("contract-crash", &Crash::make, true);
    std::string log;
    const std::vector<std::string> order{"contract-crash"};
    CHECK(ui::render::createFirstWorking(order, {}, log) == nullptr);
    CHECK(log.find("contract-crash unavailable: child killed by signal") != std::string::npos);
}
