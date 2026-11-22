// Pre-rasterised glyphs for every token text style at the device scale, packed into one 8-bit coverage atlas.
// Built with JUCE's font engine (FreeType on Linux, CoreText on macOS) from the embedded fonts —
// Inter in each style's token weight (real faces, no synthetic emboldening), tabular open digits for the value styles,
// a DejaVu subset for the symbols Inter lacks — then shared by the software rasteriser (alpha blits) and the GLES2
// backend (one GL_ALPHA texture, one draw call). No font work happens per frame: layout is a table lookup per code
// point.
//
// Start-up: loadOrBuild() reads <cacheDir>/atlas-<font>-<scale>.bin when its key (format, font bytes,
// token styles, code points, scale, JUCE version, platform) matches, otherwise builds and writes it (atomically).
// Message thread only; build/load allocate, find/measure never do.
#pragma once

#include "ui/render/DrawList.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ui
{
class GlyphAtlas
{
public:
    struct Glyph
    {
        std::uint16_t x = 0, y = 0, w = 0, h = 0; // atlas rect (pixels)
        float left = 0;                           // bitmap left edge relative to the pen position
        float top = 0;                            // bitmap top relative to the baseline (negative = above)
        float advance = 0;                        // pen advance in surface pixels, tracking included
    };
    struct StyleMetrics
    {
        float ascent = 0, descent = 0, sizePx = 0;
        float xHeight = 0; // ink height of "x" in surface px (the primary face)
        // The alphabetic baseline below a text command's centre line: CSS dominant-baseline="middle" (the mockups,
        // DrawList.h) puts the baseline half the x-height below y. Every backend places text with this.
        float baselineBelowCentre() const noexcept { return xHeight * 0.5f; }
    };
    enum class Source : std::uint8_t
    {
        None,
        Built,
        Cache
    };

    // The embedded fonts (ui/render/fonts/Fonts.h): every style in its token weight. `scale` = surface px per lpx.
    bool build(float scale, std::string& error);
    // One font for every style (skeleton, tests): weight >= 600 is emboldened synthetically; symbols the font lacks
    // come from the embedded DejaVu subset.
    bool build(float scale, const void* fontData, std::size_t fontSize, std::string& error);
    // The embedded fonts through the disk cache: loads <cacheDir>/<cacheFileName(scale)> when it is valid for this
    // build, otherwise builds and writes it. An empty cacheDir only builds. A cache that cannot be written is not an
    // error (cacheNote() says why).
    bool loadOrBuild(float scale, std::string_view cacheDir, std::string& error);

    bool save(const std::string& path, std::string& error) const;
    // Loads a cache file written for the embedded fonts at `scale`; false (and the atlas unchanged) when the file is
    // missing, truncated, corrupt or was written for other fonts, styles, scale or JUCE version.
    bool load(const std::string& path, float scale, std::string& error);
    static std::string cacheFileName(float scale); // "atlas-inter-4.1-0.625.bin"

    const Glyph* find(TextStyleId style, char32_t cp) const noexcept;
    const StyleMetrics& metrics(TextStyleId s) const noexcept { return metrics_[static_cast<std::size_t>(s)]; }
    float measure(TextStyleId style, std::string_view utf8) const noexcept; // surface pixels
    float scale() const noexcept { return scale_; }

    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }
    const std::uint8_t* pixels() const noexcept { return pixels_.data(); }

    // Provenance, for logs and tests.
    bool embeddedFonts() const noexcept { return embedded_; }
    Source source() const noexcept { return source_; }
    std::uint64_t key() const noexcept { return key_; }
    const std::string& cacheNote() const noexcept { return cacheNote_; }
    std::size_t glyphCount() const noexcept;
    std::string describe() const; // "1024x1024 inter-4.1 3104 glyphs, from cache /path"

    // Code points every style carries in a direct table; Charset.h's symbols follow in a sorted list.
    static constexpr char32_t kDirectFirst = 0x20, kDirectLast = 0x17F;

private:
    struct StyleGlyphs
    {
        std::array<Glyph, kDirectLast - kDirectFirst + 1> direct{};
        std::vector<std::pair<char32_t, Glyph>> symbols; // sorted by code point
    };
    bool buildWith(float scale, const void* singleFont, std::size_t singleSize, std::string& error);
    static std::uint64_t computeKey(float scale, const void* singleFont, std::size_t singleSize);

    std::array<StyleGlyphs, static_cast<std::size_t>(TextStyleId::Count)> styles_{};
    std::array<StyleMetrics, static_cast<std::size_t>(TextStyleId::Count)> metrics_{};
    std::vector<std::uint8_t> pixels_;
    int width_ = 0, height_ = 0;
    float scale_ = 1;
    bool embedded_ = false;
    Source source_ = Source::None;
    std::uint64_t key_ = 0;
    std::string cacheNote_, cachePath_;
};
} // namespace ui
