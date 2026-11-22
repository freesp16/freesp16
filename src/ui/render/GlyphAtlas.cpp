#include "ui/render/GlyphAtlas.h"

#include "ui/render/fonts/Charset.h"
#include "ui/render/fonts/Fonts.h"
#include "ui/theme/Theme.h"

#include <juce_graphics/juce_graphics.h>

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <type_traits>

namespace ui
{
namespace
{
constexpr std::uint32_t kFormat = 3; // bump when the file layout or the rasterisation changes
constexpr char kMagic[8] = {'S', 'X', 'A', 'T', 'L', 'A', 'S', '\0'};
constexpr std::size_t kStyles = static_cast<std::size_t>(TextStyleId::Count);
static_assert(std::is_trivially_copyable_v<GlyphAtlas::Glyph> && sizeof(GlyphAtlas::Glyph) == 20);

// FNV-1a over 64-bit words (byte tail), for the cache key and the file checksum: fast enough for ~1 MB at start-up.
struct Hash
{
    std::uint64_t h = 0xcbf29ce484222325ull;
    void bytes(const void* data, std::size_t n) noexcept
    {
        const auto* p = static_cast<const unsigned char*>(data);
        constexpr std::uint64_t prime = 0x100000001b3ull;
        for (; n >= 8; n -= 8, p += 8)
        {
            std::uint64_t w;
            std::memcpy(&w, p, 8);
            h = (h ^ w) * prime;
        }
        for (; n > 0; --n, ++p)
            h = (h ^ *p) * prime;
    }
    template <typename T> void value(const T& v) noexcept { bytes(&v, sizeof v); }
};

// Value styles never show Central European names; the largest ones skip Latin Extended-A (fewer glyphs to build).
bool coversExtendedA(TextStyleId s) noexcept
{
    return s != TextStyleId::Display && s != TextStyleId::ValueL;
}

bool inCharset(TextStyleId s, char32_t cp) noexcept
{
    if (cp >= 0x100 && cp <= 0x17F && ! coversExtendedA(s))
        return false;
    for (const auto& r : fonts::kRanges)
        if (cp >= r.first && cp <= r.last)
            return true;
    return false;
}

std::vector<char32_t> sortedSymbols()
{
    std::vector<char32_t> s(std::begin(fonts::kSymbols), std::end(fonts::kSymbols));
    std::sort(s.begin(), s.end());
    s.erase(std::unique(s.begin(), s.end()), s.end());
    return s;
}

struct Bitmap
{
    juce::Image image;
    float left = 0, top = 0, advance = 0;
    bool present = false;
};

juce::Font makeFont(const juce::Typeface::Ptr& tf, float px)
{
    // FontOptions(typeface) — not withTypeface() on a default FontOptions, whose name/style JUCE asserts are empty.
    return juce::Font(juce::FontOptions(tf).withPointHeight(px).withFallbackEnabled(false));
}

Bitmap rasterise(const juce::Font& font, char32_t cp, float tracking, float boldStroke)
{
    Bitmap b;
    const auto text = juce::String::charToString(static_cast<juce::juce_wchar>(cp));
    juce::GlyphArrangement ga;
    ga.addLineOfText(font, text, 0.0f, 0.0f); // baseline at y = 0
    b.present = true;
    b.advance = (ga.getNumGlyphs() > 0 ? ga.getBoundingBox(0, -1, true).getWidth() : 0.0f) + tracking;
    juce::Path path;
    ga.createPath(path);
    if (path.isEmpty())
        return b;
    const auto bounds = path.getBounds().expanded(1.0f + boldStroke);
    const int x0 = static_cast<int>(std::floor(bounds.getX())), y0 = static_cast<int>(std::floor(bounds.getY()));
    const int w = static_cast<int>(std::ceil(bounds.getRight())) - x0;
    const int h = static_cast<int>(std::ceil(bounds.getBottom())) - y0;
    b.image = juce::Image(juce::Image::SingleChannel, std::max(1, w), std::max(1, h), true, juce::SoftwareImageType());
    juce::Graphics g(b.image);
    g.setColour(juce::Colours::white);
    const auto t = juce::AffineTransform::translation(static_cast<float>(-x0), static_cast<float>(-y0));
    g.fillPath(path, t);
    if (boldStroke > 0)
        g.strokePath(path, juce::PathStrokeType(boldStroke), t);
    b.left = static_cast<float>(x0);
    b.top = static_cast<float>(y0);
    return b;
}

// Ink height of "x" (CSS x-height as browsers measure it for dominant-baseline="middle"; the glyph cell would give
// the ascent instead).
float xHeightOf(const juce::Font& font)
{
    juce::GlyphArrangement ga;
    ga.addLineOfText(font, "x", 0.0f, 0.0f);
    juce::Path outline;
    ga.createPath(outline);
    return outline.isEmpty() ? font.getAscent() * 0.5f : -outline.getBounds().getY();
}

std::string scaleText(float scale)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4g", static_cast<double>(scale));
    return buf;
}

bool makeDirs(const std::string& dir, std::string& error)
{
    std::string path;
    for (std::size_t i = 0; i <= dir.size(); ++i)
    {
        if (i == dir.size() || dir[i] == '/')
        {
            if (! path.empty() && ::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST)
            {
                error = "mkdir " + path + ": " + std::strerror(errno);
                return false;
            }
        }
        if (i < dir.size())
            path += dir[i];
    }
    return true;
}

} // namespace

std::uint64_t GlyphAtlas::computeKey(float scale, const void* singleFont, std::size_t singleSize)
{
    Hash k;
    k.bytes(kMagic, sizeof kMagic);
    k.value(kFormat);
    k.value(scale);
    if (singleFont != nullptr)
    {
        k.value(singleSize);
        k.bytes(singleFont, singleSize);
    }
    for (int f = 0; f < static_cast<int>(fonts::Face::Count); ++f)
    {
        const auto b = fonts::blob(static_cast<fonts::Face>(f));
        if (singleFont != nullptr && static_cast<fonts::Face>(f) != fonts::Face::Symbols)
            continue;
        k.value(b.size);
        k.bytes(b.data, b.size);
    }
    for (std::size_t s = 0; s < kStyles; ++s)
    {
        const auto& st = theme::textStyle(static_cast<TextStyleId>(s));
        k.value(st.size);
        k.value(st.weight);
        k.value(st.tracking);
        k.value(st.tabular);
    }
    for (const auto& r : fonts::kRanges)
        k.value(r);
    k.bytes(fonts::kSymbols, sizeof fonts::kSymbols);
    const auto juceVersion = juce::SystemStats::getJUCEVersion().toStdString();
    k.bytes(juceVersion.data(), juceVersion.size());
#if JUCE_MAC
    k.bytes("coretext", 8);
#else
    k.bytes("freetype", 8);
#endif
    return k.h;
}

bool GlyphAtlas::build(float scale, std::string& error)
{
    return buildWith(scale, nullptr, 0, error);
}

bool GlyphAtlas::build(float scale, const void* fontData, std::size_t fontSize, std::string& error)
{
    if (fontData == nullptr || fontSize == 0)
        return error = "glyph atlas: no font", false;
    return buildWith(scale, fontData, fontSize, error);
}

bool GlyphAtlas::buildWith(float scale, const void* singleFont, std::size_t singleSize, std::string& error)
{
    const auto symbolsBlob = fonts::blob(fonts::Face::Symbols);
    const auto symbolsTf = juce::Typeface::createSystemTypefaceFor(symbolsBlob.data, symbolsBlob.size);
    juce::Typeface::Ptr single;
    if (singleFont != nullptr && (single = juce::Typeface::createSystemTypefaceFor(singleFont, singleSize)) == nullptr)
        return error = "glyph atlas: cannot load the font", false;
    std::array<juce::Typeface::Ptr, static_cast<std::size_t>(fonts::Face::Count)> faces{};
    auto face = [&](fonts::Face f) -> juce::Typeface::Ptr
    {
        auto& tf = faces[static_cast<std::size_t>(f)];
        if (tf == nullptr)
        {
            const auto b = fonts::blob(f);
            tf = juce::Typeface::createSystemTypefaceFor(b.data, b.size);
        }
        return tf;
    };
    if (symbolsTf == nullptr)
        return error = "glyph atlas: cannot load the embedded symbol font", false;

    const auto symbols = sortedSymbols();
    struct Pending
    {
        std::size_t style;
        char32_t cp;
        Bitmap bmp;
    };
    std::vector<Pending> pending;
    pending.reserve(kStyles * 420);
    std::array<StyleMetrics, kStyles> metrics{};
    for (std::size_t s = 0; s < kStyles; ++s)
    {
        const auto id = static_cast<TextStyleId>(s);
        const auto& style = theme::textStyle(id);
        const float px = style.size * scale;
        const juce::Typeface::Ptr primary = single != nullptr ? single : face(fonts::faceForWeight(style.weight));
        if (primary == nullptr)
            return error = "glyph atlas: cannot load the embedded fonts", false;
        const juce::Font primaryFont = makeFont(primary, px), symbolFont = makeFont(symbolsTf, px);
        const float boldStroke =
            single != nullptr && style.weight >= 600 ? px * 0.045f : 0.0f; // synthetic, one font only
        metrics[s] = {primaryFont.getAscent(), primaryFont.getDescent(), px, xHeightOf(primaryFont)};
        const float tracking = style.tracking * px;

        auto add = [&](char32_t cp)
        {
            const auto wc = static_cast<juce::juce_wchar>(cp);
            Bitmap b;
            if (primary->getNominalGlyphForCodepoint(wc).has_value())
                b = rasterise(primaryFont, cp, tracking, boldStroke);
            else if (symbolsTf->getNominalGlyphForCodepoint(wc).has_value())
                b = rasterise(symbolFont, cp, tracking, 0.0f);
            pending.push_back({s, cp, std::move(b)});
        };
        for (char32_t cp = kDirectFirst; cp <= kDirectLast; ++cp)
            if (inCharset(id, cp))
                add(cp);
        for (auto cp : symbols)
            add(cp);

        // Tabular figures (tokens "tnum"): the embedded value faces have them frozen in; a single font gets them
        // emulated — every digit advances by the widest digit and is centred in that cell.
        if (style.tabular)
        {
            float widest = 0;
            for (auto& p : pending)
                if (p.style == s && p.cp >= '0' && p.cp <= '9')
                    widest = std::max(widest, p.bmp.advance);
            for (auto& p : pending)
                if (p.style == s && p.cp >= '0' && p.cp <= '9' && p.bmp.advance < widest)
                {
                    p.bmp.left += (widest - p.bmp.advance) * 0.5f;
                    p.bmp.advance = widest;
                }
        }
    }

    // Shelf packing, tallest first, 1 px gutter, fixed width.
    std::vector<std::size_t> order(pending.size());
    for (std::size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](auto a, auto b)
                     { return pending[a].bmp.image.getHeight() > pending[b].bmp.image.getHeight(); });
    const int width = scale < 0.8f ? 1024 : 2048;
    int penX = 1, penY = 1, shelfH = 0;
    std::vector<std::pair<int, int>> pos(pending.size(), {0, 0});
    for (auto i : order)
    {
        const auto& img = pending[i].bmp.image;
        if (! img.isValid())
            continue;
        if (penX + img.getWidth() + 1 > width)
        {
            penX = 1;
            penY += shelfH + 1;
            shelfH = 0;
        }
        pos[i] = {penX, penY};
        penX += img.getWidth() + 1;
        shelfH = std::max(shelfH, img.getHeight());
    }
    int height = 1;
    while (height < penY + shelfH + 1)
        height *= 2;
    if (height > 8192)
        return error = "glyph atlas: glyphs do not fit 8192 rows", false;

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
    std::array<StyleGlyphs, kStyles> styles{};
    std::array<bool, kStyles*(kDirectLast - kDirectFirst + 1)> have{};
    for (std::size_t i = 0; i < pending.size(); ++i)
    {
        auto& p = pending[i];
        Glyph g;
        g.advance = p.bmp.advance;
        if (p.bmp.image.isValid())
        {
            const juce::Image::BitmapData data(p.bmp.image, juce::Image::BitmapData::readOnly);
            for (int y = 0; y < data.height; ++y)
            {
                auto* dst = &pixels[static_cast<std::size_t>((pos[i].second + y) * width + pos[i].first)];
                for (int x = 0; x < data.width; ++x)
                    dst[x] = *data.getPixelPointer(x, y);
            }
            g.x = static_cast<std::uint16_t>(pos[i].first);
            g.y = static_cast<std::uint16_t>(pos[i].second);
            g.w = static_cast<std::uint16_t>(data.width);
            g.h = static_cast<std::uint16_t>(data.height);
            g.left = p.bmp.left;
            g.top = p.bmp.top;
        }
        auto& st = styles[p.style];
        if (p.cp >= kDirectFirst && p.cp <= kDirectLast)
        {
            st.direct[p.cp - kDirectFirst] = g;
            have[p.style * (kDirectLast - kDirectFirst + 1) + (p.cp - kDirectFirst)] = p.bmp.present;
        }
        else if (p.bmp.present)
            st.symbols.emplace_back(p.cp, g);
    }
    // Code points no font draws, and those outside the style's set, render as '?' (layout stays defined).
    for (std::size_t s = 0; s < kStyles; ++s)
    {
        auto& st = styles[s];
        const Glyph q = st.direct['?' - kDirectFirst];
        for (char32_t cp = kDirectFirst; cp <= kDirectLast; ++cp)
            if (! have[s * (kDirectLast - kDirectFirst + 1) + (cp - kDirectFirst)])
                st.direct[cp - kDirectFirst] = q;
        std::sort(st.symbols.begin(), st.symbols.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    }

    styles_ = std::move(styles);
    metrics_ = metrics;
    pixels_ = std::move(pixels);
    width_ = width;
    height_ = height;
    scale_ = scale;
    embedded_ = singleFont == nullptr;
    source_ = Source::Built;
    key_ = computeKey(scale, singleFont, singleSize);
    cachePath_.clear();
    return true;
}

std::string GlyphAtlas::cacheFileName(float scale)
{
    return "atlas-" + std::string(fonts::kSetName) + "-" + scaleText(scale) + ".bin";
}

bool GlyphAtlas::save(const std::string& path, std::string& error) const
{
    if (pixels_.empty())
        return error = "atlas not built", false;
    std::vector<unsigned char> out;
    auto put = [&out](const void* p, std::size_t n)
    {
        const auto* b = static_cast<const unsigned char*>(p);
        out.insert(out.end(), b, b + n);
    };
    const std::uint32_t endian = 0x01020304u;
    const auto w = static_cast<std::int32_t>(width_), h = static_cast<std::int32_t>(height_);
    put(kMagic, sizeof kMagic);
    put(&kFormat, 4);
    put(&endian, 4);
    put(&key_, 8);
    put(&scale_, 4);
    put(&w, 4);
    put(&h, 4);
    put(metrics_.data(), sizeof metrics_);
    for (const auto& st : styles_)
    {
        put(st.direct.data(), sizeof st.direct);
        const auto n = static_cast<std::uint32_t>(st.symbols.size());
        put(&n, 4);
        for (const auto& [cp, g] : st.symbols)
        {
            const auto c = static_cast<std::uint32_t>(cp);
            put(&c, 4);
            put(&g, sizeof g);
        }
    }
    put(pixels_.data(), pixels_.size());
    Hash sum;
    sum.bytes(out.data(), out.size());
    put(&sum.h, 8);

    const std::string tmp = path + ".tmp" + std::to_string(static_cast<long>(::getpid()));
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (f == nullptr)
        return error = "cannot write " + tmp + ": " + std::strerror(errno), false;
    const bool written = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    const bool closed = std::fclose(f) == 0;
    if (! written || ! closed || std::rename(tmp.c_str(), path.c_str()) != 0)
    {
        error = "cannot write " + path + ": " + std::strerror(errno);
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

bool GlyphAtlas::load(const std::string& path, float scale, std::string& error)
{
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
        return error = "no cache " + path, false;
    std::vector<unsigned char> in;
    unsigned char buf[65536];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;)
        in.insert(in.end(), buf, buf + n);
    std::fclose(f);

    std::size_t at = 0;
    auto get = [&](void* p, std::size_t n)
    {
        if (at + n > in.size())
            return false;
        std::memcpy(p, in.data() + at, n);
        at += n;
        return true;
    };
    char magic[8];
    std::uint32_t format = 0, endian = 0;
    std::uint64_t key = 0;
    float fileScale = 0;
    std::int32_t w = 0, h = 0;
    if (in.size() < 8 + 8 || ! get(magic, 8) || std::memcmp(magic, kMagic, 8) != 0 || ! get(&format, 4) ||
        format != kFormat || ! get(&endian, 4) || endian != 0x01020304u)
        return error = "cache " + path + ": not an atlas of this format", false;
    Hash sum;
    sum.bytes(in.data(), in.size() - 8);
    std::uint64_t stored = 0;
    std::memcpy(&stored, in.data() + in.size() - 8, 8);
    if (stored != sum.h)
        return error = "cache " + path + ": checksum mismatch", false;
    if (! get(&key, 8) || key != computeKey(scale, nullptr, 0))
        return error = "cache " + path + ": written for other fonts, styles, scale or JUCE", false;
    std::array<StyleMetrics, kStyles> metrics{};
    std::array<StyleGlyphs, kStyles> styles{};
    if (! get(&fileScale, 4) || ! get(&w, 4) || ! get(&h, 4) || w <= 0 || h <= 0 || w > 8192 || h > 8192 ||
        ! get(metrics.data(), sizeof metrics))
        return error = "cache " + path + ": truncated", false;
    for (auto& st : styles)
    {
        std::uint32_t n = 0;
        if (! get(st.direct.data(), sizeof st.direct) || ! get(&n, 4) || n > 4096)
            return error = "cache " + path + ": truncated", false;
        st.symbols.resize(n);
        for (auto& [cp, g] : st.symbols)
        {
            std::uint32_t c = 0;
            if (! get(&c, 4) || ! get(&g, sizeof g))
                return error = "cache " + path + ": truncated", false;
            cp = static_cast<char32_t>(c);
        }
    }
    const auto count = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    if (at + count + 8 != in.size())
        return error = "cache " + path + ": size mismatch", false;
    pixels_.assign(in.begin() + static_cast<std::ptrdiff_t>(at), in.begin() + static_cast<std::ptrdiff_t>(at + count));
    styles_ = std::move(styles);
    metrics_ = metrics;
    width_ = w;
    height_ = h;
    scale_ = fileScale;
    embedded_ = true;
    source_ = Source::Cache;
    key_ = key;
    cachePath_ = path;
    return true;
}

bool GlyphAtlas::loadOrBuild(float scale, std::string_view cacheDir, std::string& error)
{
    cacheNote_.clear();
    if (cacheDir.empty())
        return build(scale, error);
    std::string dir(cacheDir);
    while (dir.size() > 1 && dir.back() == '/')
        dir.pop_back();
    const std::string path = dir + "/" + cacheFileName(scale);
    std::string why;
    if (load(path, scale, why))
        return true;
    if (! build(scale, error))
        return false;
    std::string saveError;
    if (! makeDirs(dir, saveError) || ! save(path, saveError))
        cacheNote_ = why + "; " + saveError;
    else
    {
        cacheNote_ = why + "; wrote " + path;
        cachePath_ = path;
    }
    return true;
}

const GlyphAtlas::Glyph* GlyphAtlas::find(TextStyleId style, char32_t cp) const noexcept
{
    const auto& st = styles_[static_cast<std::size_t>(style)];
    if (cp >= kDirectFirst && cp <= kDirectLast)
        return &st.direct[cp - kDirectFirst];
    const auto it = std::lower_bound(st.symbols.begin(), st.symbols.end(), cp,
                                     [](const auto& e, char32_t c) { return e.first < c; });
    if (it != st.symbols.end() && it->first == cp)
        return &it->second;
    return &st.direct['?' - kDirectFirst];
}

float GlyphAtlas::measure(TextStyleId style, std::string_view utf8) const noexcept
{
    float w = 0;
    for (std::size_t i = 0; i < utf8.size();)
        w += find(style, nextCodepoint(utf8, i))->advance;
    return w;
}

std::size_t GlyphAtlas::glyphCount() const noexcept
{
    std::size_t n = 0;
    for (const auto& st : styles_)
        n += st.direct.size() + st.symbols.size();
    return n;
}

std::string GlyphAtlas::describe() const
{
    char buf[96];
    std::snprintf(buf, sizeof buf, "%dx%d %s %zu glyphs", width_, height_,
                  embedded_ ? std::string(fonts::kSetName).c_str() : "single-font", glyphCount());
    std::string s = buf;
    if (source_ == Source::Cache)
        s += ", from cache " + cachePath_;
    else if (source_ == Source::Built)
        s += ", built" + (cacheNote_.empty() ? std::string() : " (" + cacheNote_ + ")");
    return s;
}
} // namespace ui
