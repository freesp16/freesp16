#include "tests/snapshot/SnapshotTest.h"

#include "tests/fakes/OffscreenRenderer.h"

#include "ui/render/fonts/Fonts.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace snapshot
{
namespace
{
// --- the reference rasteriser --------------------------------------------------------------------------------------
// The mockups name Inter and rely on CSS text semantics (font-size = em, font-weight 300…600, letter-spacing,
// text-anchor, dominant-baseline="middle", tabular numerals for the value styles). JUCE's SVG renderer (lunasvg)
// resolves fonts through the host, maps weights to bold/regular only and ignores letter-spacing and dominant-baseline,
// so the harness rasterises the shapes with JUCE's SVG renderer and draws every <text> itself, in document order, with
// the app's embedded Inter faces (ui/render/fonts: the weight's face — Light and Medium carry tnum + ss01 frozen in, as
// the value styles ask — and the DejaVu symbol subset for code points Inter lacks), em-sized, with the letter spacing
// applied per glyph and the alphabetic baseline at y + x-height / 2 (CSS "middle").
struct Faces
{
    std::array<juce::Typeface::Ptr, static_cast<std::size_t>(ui::fonts::Face::Count)> tf;
    Faces()
    {
        for (std::size_t i = 0; i < tf.size(); ++i)
        {
            const auto b = ui::fonts::blob(static_cast<ui::fonts::Face>(i));
            tf[i] = juce::Typeface::createSystemTypefaceFor(b.data, b.size);
        }
    }
    const juce::Typeface::Ptr& operator[](ui::fonts::Face f) const { return tf[static_cast<std::size_t>(f)]; }
};

const Faces& faces()
{
    static const Faces f;
    return f;
}

juce::Font fontOf(const juce::Typeface::Ptr& tf, float px)
{
    return juce::Font(juce::FontOptions(tf).withPointHeight(px).withFallbackEnabled(false));
}

std::string attr(const std::string& element, const char* name)
{
    const std::string key = std::string(" ") + name + "=\"";
    const auto at = element.find(key);
    if (at == std::string::npos)
        return {};
    const auto start = at + key.size();
    return element.substr(start, element.find('"', start) - start);
}

juce::String unescape(const std::string& s)
{
    return juce::String::fromUTF8(s.c_str())
        .replace("&lt;", "<")
        .replace("&gt;", ">")
        .replace("&quot;", "\"")
        .replace("&apos;", "'")
        .replace("&amp;", "&");
}

juce::Colour colourOf(const std::string& hex, float opacity)
{
    if (hex.size() < 7 || hex[0] != '#')
        return juce::Colours::transparentBlack;
    const auto v = static_cast<std::uint32_t>(std::stoul(hex.substr(1), nullptr, 16));
    const bool alpha = hex.size() == 9;
    const auto argb = alpha ? ((v & 0xFFu) << 24) | (v >> 8) : 0xFF000000u | v;
    return juce::Colour(argb).withMultipliedAlpha(opacity);
}

// One <text> element, drawn with CSS semantics.
void drawText(juce::Graphics& g, const std::string& element)
{
    const auto open = element.find('>');
    const auto close = element.rfind("</text>");
    if (open == std::string::npos || close == std::string::npos || close <= open)
        return;
    const auto text = unescape(element.substr(open + 1, close - open - 1));
    const float x = std::stof(attr(element, "x")), y = std::stof(attr(element, "y"));
    const float size = std::stof(attr(element, "font-size"));
    const auto weightText = attr(element, "font-weight");
    const int weight = weightText.empty() ? 400 : std::stoi(weightText);
    const auto spacingText = attr(element, "letter-spacing");
    const float spacing = spacingText.empty() ? 0.0f : std::stof(spacingText);
    const auto anchor = attr(element, "text-anchor");
    const auto opacityText = attr(element, "opacity");
    const auto colour = colourOf(attr(element, "fill"), opacityText.empty() ? 1.0f : std::stof(opacityText));

    const auto& primary = faces()[ui::fonts::faceForWeight(weight)];
    const auto& symbols = faces()[ui::fonts::Face::Symbols];
    const auto font = fontOf(primary, size), symbolFont = fontOf(symbols, size);
    // Per code point: its font (the symbol subset where Inter has no glyph) and advance (+ letter spacing).
    struct Glyph
    {
        juce::String ch;
        const juce::Font* font;
        float advance;
    };
    std::vector<Glyph> glyphs;
    float width = 0;
    for (auto p = text.getCharPointer(); ! p.isEmpty();)
    {
        const auto c = p.getAndAdvance();
        const bool inter = primary->getNominalGlyphForCodepoint(c).has_value();
        const auto* f = inter || symbols == nullptr ? &font : &symbolFont;
        const auto ch = juce::String::charToString(c);
        const float adv = juce::GlyphArrangement::getStringWidth(*f, ch) + spacing;
        glyphs.push_back({ch, f, adv});
        width += adv;
    }
    float pen = anchor == "middle" ? x - width / 2 : (anchor == "end" ? x - width : x);
    // dominant-baseline="middle": the alphabetic baseline sits half the x-height below y.
    float baseline = y;
    if (attr(element, "dominant-baseline") == "middle")
    {
        juce::GlyphArrangement xg;
        xg.addLineOfText(font, "x", 0, 0);
        juce::Path outline; // ink bounds of "x" (the glyph boxes span the whole ascent)
        xg.createPath(outline);
        baseline = y - outline.getBounds().getY() / 2;
    }
    g.setColour(colour);
    for (const auto& gl : glyphs)
    {
        juce::GlyphArrangement ga;
        ga.addLineOfText(*gl.font, gl.ch, pen, baseline);
        ga.draw(g);
        pen += gl.advance;
    }
}

// Shapes through JUCE's SVG renderer, texts through drawText(), in document order (the generator writes one element
// per line, every element a direct child of <svg>).
bool rasterise(const juce::File& file, Image& img)
{
    std::istringstream lines(file.loadFileAsString().toStdString());
    std::string header, line;
    std::vector<std::string> elements;
    while (std::getline(lines, line))
    {
        const auto first = line.find_first_not_of(' ');
        if (first == std::string::npos)
            continue;
        line = line.substr(first);
        if (line.rfind("<svg", 0) == 0)
            header = line;
        else if (line.rfind("</svg", 0) != 0 && line.rfind("<title", 0) != 0)
            elements.push_back(line);
    }
    if (header.empty())
        return false;
    img.w = std::stoi(attr(header, "width"));
    img.h = std::stoi(attr(header, "height"));
    juce::Image raster(juce::Image::ARGB, img.w, img.h, true, juce::SoftwareImageType());
    {
        juce::Graphics g(raster);
        std::string batch;
        auto flush = [&]
        {
            if (batch.empty())
                return;
            if (auto d =
                    juce::Drawable::createFromSVGString(juce::String::fromUTF8((header + batch + "</svg>").c_str())))
                d->draw(g, 1.0f); // the viewBox equals the size: canvas coordinates 1:1 (a partial batch must not be
                                  // fitted to its own bounds)
            batch.clear();
        };
        for (const auto& e : elements)
        {
            if (e.rfind("<text", 0) == 0)
            {
                flush();
                drawText(g, e);
            }
            else
                batch += e + "\n";
        }
        flush();
    }
    juce::Image::BitmapData data(raster, juce::Image::BitmapData::readOnly);
    img.px.resize(static_cast<std::size_t>(img.w * img.h));
    for (int y = 0; y < img.h; ++y)
        for (int x = 0; x < img.w; ++x)
            img.px[static_cast<std::size_t>(y * img.w + x)] = data.getPixelColour(x, y).getARGB() & 0x00FFFFFFu;
    return true;
}

int& sessions()
{
    static int n = 0;
    return n;
}
void gui()
{
    if (sessions() == 0)
        std::fprintf(stderr, "snapshot: create a snapshot::Session in the test case first\n");
}

fakes::OffscreenRenderer& offscreen(int w, int h)
{
    static std::map<std::pair<int, int>, std::unique_ptr<fakes::OffscreenRenderer>> cache;
    auto& slot = cache[{w, h}];
    if (! slot)
    {
        slot = std::make_unique<fakes::OffscreenRenderer>();
        std::string error;
        if (! slot->open(w, h, error))
            std::fprintf(stderr, "snapshot: offscreen renderer %dx%d: %s\n", w, h, error.c_str());
    }
    return *slot;
}

juce::Image toJuce(const Image& img)
{
    juce::Image out(juce::Image::RGB, img.w, img.h, false, juce::SoftwareImageType());
    juce::Image::BitmapData data(out, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < img.h; ++y)
        for (int x = 0; x < img.w; ++x)
            data.setPixelColour(x, y, juce::Colour(0xFF000000u | img.at(x, y)));
    return out;
}

// Box blur of one channel over a region via a summed-area table: out[y][x] = mean of the 5 × 5 neighbourhood
// (clamped at the region's edge).
std::vector<int> blurred(const Image& img, ui::RectF r, int shift)
{
    const int x0 = static_cast<int>(r.x), y0 = static_cast<int>(r.y), w = static_cast<int>(r.w),
              h = static_cast<int>(r.h);
    std::vector<long> sat(static_cast<std::size_t>((w + 1) * (h + 1)), 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const int px = std::clamp(x0 + x, 0, img.w - 1), py = std::clamp(y0 + y, 0, img.h - 1);
            const long v = static_cast<long>((img.at(px, py) >> shift) & 255u);
            sat[static_cast<std::size_t>((y + 1) * (w + 1) + x + 1)] =
                v + sat[static_cast<std::size_t>(y * (w + 1) + x + 1)] +
                sat[static_cast<std::size_t>((y + 1) * (w + 1) + x)] - sat[static_cast<std::size_t>(y * (w + 1) + x)];
        }
    std::vector<int> out(static_cast<std::size_t>(w * h));
    const int k = kBlur / 2;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const int ax = std::max(0, x - k), bx = std::min(w, x + k + 1), ay = std::max(0, y - k),
                      by = std::min(h, y + k + 1);
            const long s =
                sat[static_cast<std::size_t>(by * (w + 1) + bx)] - sat[static_cast<std::size_t>(ay * (w + 1) + bx)] -
                sat[static_cast<std::size_t>(by * (w + 1) + ax)] + sat[static_cast<std::size_t>(ay * (w + 1) + ax)];
            out[static_cast<std::size_t>(y * w + x)] = static_cast<int>(s / ((bx - ax) * (by - ay)));
        }
    return out;
}

Image crop(const Image& img, ui::RectF r)
{
    Image out;
    out.w = static_cast<int>(r.w);
    out.h = static_cast<int>(r.h);
    out.px.resize(static_cast<std::size_t>(out.w * out.h));
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x)
            out.px[static_cast<std::size_t>(y * out.w + x)] =
                img.at(std::clamp(static_cast<int>(r.x) + x, 0, img.w - 1),
                       std::clamp(static_cast<int>(r.y) + y, 0, img.h - 1));
    return out;
}
} // namespace

struct Session::Impl
{
    juce::ScopedJuceInitialiser_GUI init;
};

Session::Session() : impl_(new Impl)
{
    ++sessions();
}

Session::~Session()
{
    --sessions();
    delete impl_;
}

const ui::TextMeasure& measure()
{
    static const ui::TextMeasure m = offscreen(1280, 768).measure();
    return m;
}

Image render(const ui::DrawList& dl, int w, int h)
{
    Image img;
    img.w = w;
    img.h = h;
    img.px = offscreen(w, h).draw(dl);
    return img;
}

const Image& mockup(std::string_view name)
{
    static std::map<std::string, Image> cache;
    const std::string key(name);
    if (auto it = cache.find(key); it != cache.end())
        return it->second;
    gui();
    Image img;
    const auto file =
        juce::File(APP_SOURCE_DIR).getChildFile("resources/design/mockups").getChildFile(juce::String(key) + ".svg");
    if (! rasterise(file, img))
        img.px.clear();
    if (img.px.empty())
        std::fprintf(stderr, "snapshot: cannot rasterise %s\n", file.getFullPathName().toRawUTF8());
    return cache.emplace(key, std::move(img)).first->second;
}

void writePng(const Image& img, std::string_view fileName)
{
    gui();
    const juce::File dir(APP_ARTIFACT_DIR);
    dir.createDirectory();
    const auto file = dir.getChildFile(juce::String(std::string(fileName)));
    file.deleteFile();
    juce::FileOutputStream out(file);
    if (out.openedOk())
        juce::PNGImageFormat().writeImageToStream(toJuce(img), out);
}

Result compare(const Image& ours, ui::RectF oursRect, const Image& mock, ui::RectF mockRect, std::string_view name)
{
    Result r;
    if (ours.px.empty() || mock.px.empty() || oursRect.w != mockRect.w || oursRect.h != mockRect.h)
        return r;
    const int w = static_cast<int>(oursRect.w), h = static_cast<int>(oursRect.h);
    Image diff;
    diff.w = w;
    diff.h = h;
    diff.px.assign(static_cast<std::size_t>(w * h), 0);
    std::vector<int> worst(static_cast<std::size_t>(w * h), 0);
    for (int shift : {16, 8, 0})
    {
        const auto a = blurred(ours, oursRect, shift), b = blurred(mock, mockRect, shift);
        for (std::size_t i = 0; i < a.size(); ++i)
            worst[i] = std::max(worst[i], std::abs(a[i] - b[i]));
    }
    for (std::size_t i = 0; i < worst.size(); ++i)
    {
        r.within += worst[i] <= kTolerance;
        const auto g = static_cast<std::uint32_t>(std::min(255, worst[i] * 4));
        diff.px[i] = worst[i] <= kTolerance ? (g << 16 | g << 8 | g) / 4 : 0xFF0000u | (g / 2) << 8;
    }
    r.total = static_cast<long>(worst.size());
    r.match = static_cast<double>(r.within) / static_cast<double>(r.total);
    const std::string n(name);
    writePng(crop(ours, oursRect), n + "-ours.png");
    writePng(crop(mock, mockRect), n + "-mockup.png");
    writePng(diff, n + "-diff.png");
    std::printf("SNAPSHOT %s: %.2f %% of pixels within tolerance (gate %.0f %%)\n", n.c_str(), r.match * 100.0,
                kGate * 100);
    return r;
}
} // namespace snapshot
