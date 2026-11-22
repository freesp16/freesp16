// The X11 side of the imx6 and linux displays (was ui/render/device/Device.h, then platform/imx6/display): one window,
// its software presenter, the native handles EGL needs.
//  - imx6: an X server without a window manager (Xvfb in the emulator): an override-redirect, borderless window
//    covering the screen, blank cursor. The Vivante GC2000 user-space driver comes in window-system flavours (-fb,
//    -x11, ...) that must never be mixed in one process (sp16-custom-app/common/gl.c); gldispatch binds EGL/GLES to
//    one.
//  - linux: a normal managed window on the desktop (title, fixed size, WM_DELETE_WINDOW, visible cursor) that also
//    receives the keyboard; platform/linux/input polls its events on the UI thread.
#pragma once

#include "ui/render/Renderer.h"

#include <memory>
#include <string>

namespace platform::common
{
struct X11Window; // opaque: Display*, Window, visual

struct X11WindowOptions
{
    bool inputOnly = false;    // an InputOnly window (gl-fb draws to the framebuffer itself)
    bool managed = false;      // false: override-redirect, borderless, covering the screen, blank cursor (imx6)
    int width = 0, height = 0; // managed: fixed client size (0 = the screen's)
    std::string title{};       // managed: WM_NAME / _NET_WM_NAME
};

// Opens the default display and maps the window. For a managed window it also selects keyboard, exposure and
// structure events, registers WM_DELETE_WINDOW and waits (up to 2 s) until the window manager mapped it. Null +
// error on failure.
std::shared_ptr<X11Window> openX11Window(const X11WindowOptions&, std::string& error);
// The imx6 window: borderless, override-redirect, covering the screen, blank cursor (inputOnly: an InputOnly window).
std::shared_ptr<X11Window> openX11Window(bool inputOnly, std::string& error);
void x11Size(const X11Window&, int& w, int& h);
// Size of the default X screen without opening a window (for the glyph atlas scale). False when no display.
bool x11ScreenSize(int& w, int& h);
// Native handles for EGL (x11 flavour) and for an event reader on the same connection (same thread as the renderer).
void* x11Display(const X11Window&);
unsigned long x11Drawable(const X11Window&);
unsigned long x11WmDeleteAtom(const X11Window&); // 0 for an unmanaged window
// The next present() / presentRects() of the software presenter sends the whole frame (after an Expose).
void x11RequestFullPresent(X11Window&) noexcept;

// Software presenter: an XImage the rasteriser writes into directly when the visual is 24/32-bit XRGB (zero copy),
// otherwise an XRGB buffer converted on present. present() = XPutImage + XSync (the stock JUCE 3 app's path).
std::unique_ptr<ui::PixelPresenter> makeX11Presenter(std::shared_ptr<X11Window>, std::string& error);
} // namespace platform::common
