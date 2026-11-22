// Replays a DrawList through juce::Graphics. Shared by the two JUCE backends so they draw identically:
//   juce      JUCE's software renderer into a juce::Image (CPU), presented by the app's X11 presenter
//   juce-gl   JUCE's OpenGL renderer (createOpenGLGraphicsContext) inside a JUCE 9 OpenGLContext on EGL/GLES2 (X11)
// Fonts: one given font for every style, or (fontData null) the embedded Inter faces in each style's token weight —
// the same faces as the glyph atlas.
#pragma once

#include "ui/render/DrawList.h"

#include <juce_graphics/juce_graphics.h>

#include <array>
#include <vector>

namespace ui
{
class JuceReplay
{
public:
    JuceReplay(const void* fontData, std::size_t fontSize);
    // Draws `list` (canvas lpx) into g, scaled uniformly to a surface of width x height.
    void draw(juce::Graphics& g, const DrawList& list, int width, int height);

private:
    std::array<juce::Typeface::Ptr, static_cast<std::size_t>(TextStyleId::Count)> typefaces_;
    std::vector<juce::Font> fonts_; // per TextStyleId (juce::Font has no non-deprecated default constructor)
    std::vector<float> xHalf_;      // per TextStyleId: half the x-height, canvas lpx
};
} // namespace ui
