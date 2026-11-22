// Glyph atlas disk cache (start-up budget): <cacheDir>/atlas-<font>-<scale>.bin round-trips exactly, a second
// start loads it instead of building (budget: <= 100 ms on the device; the host must be far below), and any mismatch
// or corruption falls back to building and rewrites the file.
#include "ui/render/GlyphAtlas.h"
#include "ui/render/fonts/Fonts.h"

#include <juce_core/juce_core.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <fstream>

namespace
{
std::string tempDir()
{
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                   .getChildFile("render-atlas-cache-" + juce::String(juce::Random::getSystemRandom().nextInt64()));
    return dir.getFullPathName().toStdString() + "/nested/cache";
}

double ms(std::chrono::steady_clock::time_point a)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
}

void sameAtlas(const ui::GlyphAtlas& a, const ui::GlyphAtlas& b)
{
    REQUIRE(a.width() == b.width());
    REQUIRE(a.height() == b.height());
    CHECK(std::equal(a.pixels(), a.pixels() + a.width() * a.height(), b.pixels()));
    for (int s = 0; s < static_cast<int>(ui::TextStyleId::Count); ++s)
    {
        const auto st = static_cast<ui::TextStyleId>(s);
        CHECK(a.metrics(st).ascent == b.metrics(st).ascent);
        for (char32_t cp : {U'A', U'g', U'é', U'✕', U'⟲', U'?'})
        {
            const auto *x = a.find(st, cp), *y = b.find(st, cp);
            CHECK((x->x == y->x && x->y == y->y && x->w == y->w && x->h == y->h && x->advance == y->advance &&
                   x->left == y->left && x->top == y->top));
        }
    }
}
} // namespace

TEST_CASE("render atlas cache: written on the first start, loaded on the second", "[render][atlas]")
{
    const auto dir = tempDir();
    std::string error;
    ui::GlyphAtlas first;
    auto t0 = std::chrono::steady_clock::now();
    REQUIRE(first.loadOrBuild(0.625f, dir, error));
    const double buildMs = ms(t0);
    CHECK(first.source() == ui::GlyphAtlas::Source::Built);
    const auto path = dir + "/" + ui::GlyphAtlas::cacheFileName(0.625f);
    CHECK(ui::GlyphAtlas::cacheFileName(0.625f) == "atlas-inter-4.1-0.625.bin");
    REQUIRE(juce::File(path).existsAsFile());

    ui::GlyphAtlas second;
    t0 = std::chrono::steady_clock::now();
    REQUIRE(second.loadOrBuild(0.625f, dir, error));
    const double loadMs = ms(t0);
    CHECK(second.source() == ui::GlyphAtlas::Source::Cache);
    CHECK(second.key() == first.key());
    sameAtlas(first, second);
    std::printf("RENDER atlas %s: built %.1f ms, from cache %.1f ms\n", second.describe().c_str(), buildMs, loadMs);
    CHECK(loadMs < 100.0);
    CHECK(loadMs * 5 < buildMs);

    // Another scale is another file; the first one stays valid.
    ui::GlyphAtlas big;
    REQUIRE(big.loadOrBuild(1.0f, dir, error));
    CHECK(big.source() == ui::GlyphAtlas::Source::Built);
    CHECK(big.width() == 2048);
    ui::GlyphAtlas again;
    REQUIRE(again.loadOrBuild(0.625f, dir, error));
    CHECK(again.source() == ui::GlyphAtlas::Source::Cache);
    juce::File(dir).getParentDirectory().getParentDirectory().deleteRecursively();
}

TEST_CASE("render atlas cache: corrupt, truncated or foreign files are rejected and rebuilt", "[render][atlas]")
{
    const auto dir = tempDir();
    std::string error;
    ui::GlyphAtlas a;
    REQUIRE(a.loadOrBuild(0.625f, dir, error));
    const auto path = dir + "/" + ui::GlyphAtlas::cacheFileName(0.625f);
    const auto good = juce::File(path).getSize();

    { // flip one pixel byte
        juce::FileOutputStream out{juce::File(path)};
        REQUIRE(out.openedOk());
        out.setPosition(good / 2);
        out.writeByte(0x5A);
    }
    ui::GlyphAtlas b;
    CHECK_FALSE(b.load(path, 0.625f, error));
    CHECK(error.find("checksum") != std::string::npos);
    REQUIRE(b.loadOrBuild(0.625f, dir, error));
    CHECK(b.source() == ui::GlyphAtlas::Source::Built);
    CHECK(juce::File(path).getSize() == good); // rewritten

    ui::GlyphAtlas c;
    CHECK_FALSE(c.load(path, 0.5f, error)); // written for another scale
    CHECK(error.find("other fonts") != std::string::npos);

    juce::File(path).replaceWithText("not an atlas");
    CHECK_FALSE(c.load(path, 0.625f, error));
    CHECK_FALSE(c.load(dir + "/missing.bin", 0.625f, error));
    CHECK(c.source() == ui::GlyphAtlas::Source::None);

    // A cache directory that cannot be created is not fatal.
    ui::GlyphAtlas d;
    REQUIRE(d.loadOrBuild(0.625f, "/dev/null/cache", error));
    CHECK(d.source() == ui::GlyphAtlas::Source::Built);
    CHECK_FALSE(d.cacheNote().empty());
    juce::File(dir).getParentDirectory().getParentDirectory().deleteRecursively();
}

TEST_CASE("render atlas: the single-font build keeps the skeleton API working", "[render][atlas]")
{
    const auto mono = ui::fonts::blob(ui::fonts::Face::Mono); // any one font file
    ui::GlyphAtlas atlas;
    std::string error;
    REQUIRE(atlas.build(0.625f, mono.data, mono.size, error));
    CHECK_FALSE(atlas.embeddedFonts());
    CHECK(atlas.find(ui::TextStyleId::Value, '1')->advance == atlas.find(ui::TextStyleId::Value, '0')->advance);
    CHECK(atlas.find(ui::TextStyleId::Body, U'✕')->w > 0);
}
