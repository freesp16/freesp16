// What a renderer backend needs from the machine: the panel size, the glyph atlas and font, the GL loader and native
// window handles. A platform::DisplayDevice fills it (platform/<id>/display/); ui/render never includes platform/** and
// never opens a display itself, so the same backend code runs on every platform that can hand it the handles (e.g. a
// future gl-wl or gl-kms backend).
#pragma once

#include "ui/render/Renderer.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace ui
{
// A native window (or framebuffer surface) for EGL. `display` is the EGLNativeDisplayType (X11 Display*, Vivante fb
// display), `window` the EGLNativeWindowType (X11 Window id or fb window pointer). `owner` keeps it alive.
struct NativeWindow
{
    void* display = nullptr;
    std::uintptr_t window = 0;
    int width = 0, height = 0;
    std::string info{}; // for describe(): "fb0 800x480 16bpp virtual 800x960"
    std::shared_ptr<void> owner{};
};

struct RendererEnv
{
    int width = 800, height = 480;     // native panel pixels
    const GlyphAtlas* atlas = nullptr; // built by the app at the panel's scale (soft, gl-*)
    const void* fontData = nullptr;    // null (default): juce, juce-gl draw with the embedded Inter faces; a font here
    std::size_t fontSize = 0;          // is used only together with a single-font atlas built from it
    // platform::Paths::cacheDir (the app copies it here): the glyph atlas cache, <cacheDir>/atlas-<font>-<scale>.bin.
    // Empty = no disk cache.
    std::string cacheDir{};

    // Binds the GL/EGL driver flavour ("fb", "x11", …) for this process; one flavour per process (gldispatch).
    std::function<bool(std::string_view flavour, std::string& error)> loadGl;
    std::function<std::string()> glReport; // one line: flavour, bound/missing counts, EGL fallbacks
    // A native surface of the given kind: "x11" (shared window), "x11-input" (input-only window), "fb" (framebuffer;
    // call after loadGl("fb")). Empty optional + error when the platform has none.
    std::function<std::optional<NativeWindow>(std::string_view kind, std::string& error)> nativeWindow;
    // The presenter CPU backends draw into: an X11 window on the device, memory on the host and in tests.
    std::function<std::unique_ptr<PixelPresenter>(std::string& error)> windowPresenter;
};
} // namespace ui
