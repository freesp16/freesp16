// Snapshot harness: renders a DrawList through OffscreenRenderer (= `soft` + memory
// presenter) at 1280 × 768 or 800 × 480, rasterises the spec's mockup (resources/design/mockups/<name>.svg at the SVG's
// own size: shapes by JUCE's SVG Drawable, texts with CSS semantics in the embedded Inter faces — SnapshotTest.cpp) and
// compares a region of one with a region of the other:
//   both images blurred with a 5 × 5 box filter; a pixel matches when its maximum channel difference is ≤ 24; a region
//   passes at ≥ 97 % matching pixels (kGate).
// compare() writes <name>-ours.png, <name>-mockup.png and <name>-diff.png to build/<preset>/artifacts/ (CI uploads
// them when a test fails) and prints one "SNAPSHOT <name>: x %" line. Golden images are never device captures:
// the reference is always the spec's SVG.
#pragma once

#include "ui/render/DrawList.h"
#include "ui/widgets/Widgets.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace snapshot
{
inline constexpr double kGate = 0.97;
inline constexpr int kTolerance = 24, kBlur = 5;

// Keeps JUCE's GUI singletons (SVG rasterising, PNG output) alive for one test case and makes the mockups render with
// the embedded font. Create one at the top of every snapshot test case.
class Session
{
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

private:
    struct Impl;
    Impl* impl_;
};

struct Image
{
    int w = 0, h = 0;
    std::vector<std::uint32_t> px; // 0x00RRGGBB, row-major
    std::uint32_t at(int x, int y) const { return px[static_cast<std::size_t>(y * w + x)]; }
};

struct Result
{
    double match = 0;
    long within = 0, total = 0;
};

// Canvas text metrics of the 1280 × 768 atlas (what the shell measures with in the snapshot tests).
const ui::TextMeasure& measure();
// Renders `dl` with the soft backend at w × h (the atlas is built once per size).
Image render(const ui::DrawList& dl, int w = 1280, int h = 768);
// The rasterised mockup resources/design/mockups/<name>.svg at its native size (cached).
const Image& mockup(std::string_view name);
// Compares ours[oursRect] with mock[mockRect] (same size, canvas pixels) under the stated tolerance.
Result compare(const Image& ours, ui::RectF oursRect, const Image& mock, ui::RectF mockRect, std::string_view name);
inline Result compare(const Image& ours, const Image& mock, ui::RectF rect, std::string_view name)
{
    return compare(ours, rect, mock, rect, name);
}
void writePng(const Image&, std::string_view fileName);
} // namespace snapshot
