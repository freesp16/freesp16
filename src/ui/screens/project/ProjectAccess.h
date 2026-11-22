// Test access to the PROJECT screens (tests/snapshot/m2): the focused row of PROJECT / OPEN / DELETE PROJECT or the
// focused control of RENDER AUDIO (-1 for another body), and a reset of the screens' session memory (last row, sort,
// render targets).
#pragma once

#include "ui/widgets/Widget.h"

namespace ui
{
int projectFocus(Widget* body);
void resetProjectScreens();
} // namespace ui
