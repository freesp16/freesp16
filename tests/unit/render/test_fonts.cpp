// Embedded fonts and the glyph atlas built from them: the compiled-in bytes are the committed subsets, every
// face loads, styles use real weights (no synthetic emboldening), value styles have tabular digits, and every code
// point of the charset (the spec's screens and mockups, the UI framework's glyph set: “ ” ✕ ✓ …) has a real glyph.
#include "ui/render/GlyphAtlas.h"
#include "ui/render/fonts/Charset.h"
#include "ui/render/fonts/Fonts.h"

#include <juce_graphics/juce_graphics.h>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace
{
std::vector<char> readFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

long ink(const ui::GlyphAtlas& atlas, ui::TextStyleId s, char32_t cp)
{
    const auto* g = atlas.find(s, cp);
    long sum = 0;
    for (int y = 0; y < g->h; ++y)
        for (int x = 0; x < g->w; ++x)
            sum += atlas.pixels()[static_cast<std::size_t>((g->y + y) * atlas.width() + g->x + x)];
    return sum;
}

const ui::GlyphAtlas& panelAtlas()
{
    static ui::GlyphAtlas atlas;
    static bool built = []
    {
        std::string error;
        return atlas.build(0.625f, error);
    }();
    REQUIRE(built);
    return atlas;
}
} // namespace

TEST_CASE("render fonts: the compiled-in font data is the committed subset files", "[render][fonts]")
{
    for (int f = 0; f < static_cast<int>(ui::fonts::Face::Count); ++f)
    {
        const auto b = ui::fonts::blob(static_cast<ui::fonts::Face>(f));
        INFO(std::string(b.name));
        const auto file =
            readFile(std::string(APP_SOURCE_DIR) + "/resources/fonts/" + std::string(b.name) + ".subset.ttf");
        REQUIRE(file.size() == b.size);
        CHECK(std::memcmp(file.data(), b.data, b.size) == 0);
        auto tf = juce::Typeface::createSystemTypefaceFor(b.data, b.size);
        REQUIRE(tf != nullptr);
        CHECK(tf->getNominalGlyphForCodepoint('A').has_value() ==
              (static_cast<ui::fonts::Face>(f) != ui::fonts::Face::Symbols));
    }
    CHECK(ui::fonts::faceForWeight(300) == ui::fonts::Face::InterLight);
    CHECK(ui::fonts::faceForWeight(400) == ui::fonts::Face::InterRegular);
    CHECK(ui::fonts::faceForWeight(500) == ui::fonts::Face::InterMedium);
    CHECK(ui::fonts::faceForWeight(600) == ui::fonts::Face::InterSemiBold);
}

TEST_CASE("render fonts: JetBrains Mono is monospaced", "[render][fonts]")
{
    const auto b = ui::fonts::blob(ui::fonts::Face::Mono);
    auto tf = juce::Typeface::createSystemTypefaceFor(b.data, b.size);
    const juce::Font font(juce::FontOptions(tf).withPointHeight(13.0f).withFallbackEnabled(false));
    CHECK(juce::GlyphArrangement::getStringWidth(font, "iiii") == juce::GlyphArrangement::getStringWidth(font, "MMMM"));
}

TEST_CASE("render fonts: styles use their real weights, value styles tabular digits", "[render][fonts]")
{
    const auto& atlas = panelAtlas();
    CHECK(atlas.embeddedFonts());
    // Body (400) and BodyStrong (600) have the same size: the semibold face has more ink.
    CHECK(ink(atlas, ui::TextStyleId::BodyStrong, 'H') > ink(atlas, ui::TextStyleId::Body, 'H') * 11 / 10);
    for (auto s : {ui::TextStyleId::ValueS, ui::TextStyleId::Value, ui::TextStyleId::ValueL, ui::TextStyleId::Display})
    {
        const float one = atlas.find(s, '1')->advance;
        for (char32_t d = '0'; d <= '9'; ++d)
            CHECK(atlas.find(s, d)->advance == one);
    }
    // Body is proportional (no tnum).
    CHECK(atlas.find(ui::TextStyleId::Body, '1')->advance < atlas.find(ui::TextStyleId::Body, '0')->advance);
}

TEST_CASE("render fonts: every charset code point has a glyph in every style", "[render][fonts]")
{
    const auto& atlas = panelAtlas();
    for (int s = 0; s < static_cast<int>(ui::TextStyleId::Count); ++s)
    {
        const auto style = static_cast<ui::TextStyleId>(s);
        const auto* q = atlas.find(style, '?');
        auto check = [&](char32_t cp)
        {
            INFO("style " << s << " U+" << std::hex << static_cast<unsigned>(cp));
            const auto* g = atlas.find(style, cp);
            CHECK(g != q);
            CHECK((g->advance > 0 || cp == 0xAD)); // soft hyphen: zero width
        };
        for (const auto& r : ui::fonts::kRanges)
            for (char32_t cp = r.first; cp <= r.last; ++cp)
                if (cp != '?' &&
                    ! (cp >= 0x100 && (style == ui::TextStyleId::Display || style == ui::TextStyleId::ValueL)))
                    check(cp);
        for (auto cp : ui::fonts::kSymbols)
            check(cp);
        // The UI framework's text glyphs: quotes, close, check — drawn, not blank.
        for (char32_t cp : {U'“', U'”', U'✕', U'✓', U'›', U'⟲'})
            CHECK(atlas.find(style, cp)->w > 0);
        CHECK(atlas.find(style, U'一') == q); // outside the charset: '?'
    }
    CHECK(atlas.measure(ui::TextStyleId::Body, "\xE2\x9C\x95") > 0);
}
