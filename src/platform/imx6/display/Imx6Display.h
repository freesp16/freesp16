// The imx6 display: an 800 × 480 panel behind an X server without a window manager, and the
// Vivante GC2000 driver in window-system flavours bound by gldispatch. Hands ui/render the native handles: the shared
// X11 window (soft, juce, gl-x11), the Vivante framebuffer window (gl-fb) and the GL loader.
#pragma once

#include "platform/Platform.h"

namespace platform::imx6
{
std::unique_ptr<DisplayDevice> makeDisplay(std::string& error);
}
