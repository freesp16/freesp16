// Text placement (DrawList.h, the mockups' dominant-baseline="middle"): a text command's y is the centre of the
// x-height — the alphabetic baseline sits half the x-height below it. Checked on the pixels of every CPU backend for
// every style: the ink of "x" is centred on y within a pixel.
#include "ui/render/GlyphAtlas.h"
#include "ui/render/Renderer.h"

#include <juce_events/juce_events.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>

TEST_CASE("render text: every backend centres the x-height on the command's y", "[render][text]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ui::GlyphAtlas atlas;
    std::string error;
    REQUIRE(atlas.build(1.0f, error)); // 1280 x 768: canvas lpx = pixels
    for (const bool juceBackend : {false, true})
        for (int s = 0; s < static_cast<int>(ui::TextStyleId::Count); ++s)
        {
            const auto style = static_cast<ui::TextStyleId>(s);
            INFO((juceBackend ? "juce" : "soft") << " style " << s);
            const auto& m = atlas.metrics(style);
            CHECK(m.xHeight > m.sizePx * 0.4f); // Inter's x-height is ~0.55 em
            CHECK(m.xHeight < m.sizePx * 0.7f);
            ui::DrawList dl;
            dl.fill({0, 0, 1280, 768}, 0xFF000000u);
            dl.text(640, 300.0f, ui::Align::Centre, style, 0xFFFFFFFFu, "xxxx");
            auto raster = juceBackend ? ui::makeJuceRasterizer(nullptr, 0) : ui::makeSoftRasterizer(atlas);
            auto r = ui::makeCpuRenderer(std::move(raster), ui::makeMemoryPresenter(1280, 768), &atlas);
            ui::FrameTiming t;
            REQUIRE(r->render(dl, t));
            std::vector<std::uint32_t> px;
            REQUIRE(r->readPixels(px));
            // Coverage-weighted vertical extent of the ink: first and last rows with any coverage, refined by the
            // partial coverage of those edge rows.
            double top = -1, bottom = -1;
            for (int y = 200; y < 400; ++y)
            {
                int peak = 0;
                for (int x = 560; x < 720; ++x)
                    peak = std::max(peak, static_cast<int>((px[static_cast<std::size_t>(y * 1280 + x)] >> 8) & 255u));
                if (peak > 0)
                {
                    if (top < 0)
                        top = y + 1.0 - peak / 255.0;
                    bottom = y + peak / 255.0;
                }
            }
            REQUIRE(top >= 0);
            const double centre = (top + bottom) * 0.5;
            CHECK(std::abs(centre - 300.0) <= 1.0);
            CHECK(std::abs((bottom - top) - static_cast<double>(m.xHeight)) <= 1.5);
        }
}
