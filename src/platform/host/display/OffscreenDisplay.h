// The host platform's display: an offscreen 800 × 480 surface (memory presenter), renderer order "soft"
// then "juce". No GL, no native window: the GL backends are device-only. The host preview window adds a
// JUCE window presenter here.
#pragma once

#include "platform/Platform.h"

namespace platform::host
{
std::unique_ptr<DisplayDevice> makeOffscreenDisplay(int width = 800, int height = 480);
}
