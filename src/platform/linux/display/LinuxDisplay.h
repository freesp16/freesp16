// The linux display: a normal managed X11 window of 800 × 480 × `scale` pixels on the desktop (title =
// identity::displayName, fixed size, WM_DELETE_WINDOW = quit, visible cursor), opened lazily and shared by soft and
// gl-x11 within one process (a probe child opens its own; the parent never opens X before the GL probes ran). The
// window also carries the keyboard and the mouse (platform/linux/input/X11Input), which reads the same shared holder.
//
// Renderers: soft (the X11 presenter shared with imx6, platform/common/display) and gl-x11 — the device's GLES2
// renderer through gldispatch with the Mesa flavour (libEGL.so.1 + libGLESv2.so.2, EGL_PLATFORM=x11), when the app
// carries the gldispatch stubs (it does on a Linux host with EGL/GLES2 headers; the test executables do not).
// juce-gl is not offered: the host JUCE library has no juce_opengl.
#pragma once

#include "platform/Platform.h"
#include "platform/common/display/X11Window.h"

#include <memory>
#include <string>

namespace platform::linux_desktop
{
struct LinuxWindow
{
    std::shared_ptr<common::X11Window> x; // null until a renderer asked for it
    int scale = 1;
    std::string title{};
    std::shared_ptr<common::X11Window> open(std::string& error);
};

std::unique_ptr<DisplayDevice> makeLinuxDisplay(std::shared_ptr<LinuxWindow> window);
} // namespace platform::linux_desktop
